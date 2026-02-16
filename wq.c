#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/hashtable.h>

#include "pacct.h"

#define PACCT_SETUP_BUDGET 32
#define ENERGY_ESTIMATE_PERIOD_MS 1

extern struct list_head traced_tasks;
extern struct list_head retiring_traced_tasks;
extern spinlock_t traced_tasks_lock;

static atomic_t estimator_enabled = ATOMIC_INIT(0);

static bool pick_one_not_ready_candidate(struct traced_task **out)
{
	struct traced_task *e;

	*out = NULL;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry(e, &traced_tasks, list) {
		if (!READ_ONCE(e->ready) && READ_ONCE(e->needs_setup)) {
			WRITE_ONCE(e->needs_setup, false);
			kref_get(&e->ref_count);
			*out = e;
			break;
		}
	}
	spin_unlock(&traced_tasks_lock);

	return *out != NULL;
}

static void pacct_setup_workfn(struct work_struct *work)
{
	int done = 0;

	for (; done < PACCT_SETUP_BUDGET; done++) {
		struct traced_task *e;

		if (!pick_one_not_ready_candidate(&e))
			break;

		WRITE_ONCE(e->ready, setup_traced_task_counters(e) == 0);
		kref_put(&e->ref_count, release_traced_task);

		cond_resched();
	}
}

static DECLARE_WORK(pacct_setup_work, pacct_setup_workfn);

void queue_pacct_setup_work(void)
{
	queue_work(system_unbound_wq, &pacct_setup_work);
}

static void pacct_retire_workfn(struct work_struct *work)
{
	struct traced_task *e;

	for (;;) {
		spin_lock(&traced_tasks_lock);

		if (list_empty(&retiring_traced_tasks)) {
			spin_unlock(&traced_tasks_lock);
			break;
		}

		e = list_first_entry(&retiring_traced_tasks, struct traced_task,
				     retire_node);
		list_del_init(&e->retire_node);
		spin_unlock(&traced_tasks_lock);

		kref_put(&e->ref_count, release_traced_task);

		cond_resched();
	}
}

static DECLARE_WORK(pacct_retire_work, pacct_retire_workfn);

void queue_pacct_retire_work(void)
{
	queue_work(system_unbound_wq, &pacct_retire_work);
}

static void pacct_energy_estimate_workfn(struct work_struct *work)
{
	struct delayed_work *dwork =
		container_of(work, struct delayed_work, work);

	struct traced_task *e, *n;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(e, n, &traced_tasks, list) {
		kref_get(&e->ref_count);

		if (!READ_ONCE(e->ready) || READ_ONCE(e->energy_updated)) {
			kref_put(&e->ref_count, release_traced_task);
			continue;
		}

		spin_unlock(&traced_tasks_lock);

		// Calculate energy estimation based on diff_counts and coefficients
		__int128 acc = 0;
		for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
			u64 diff = READ_ONCE(e->diff_counts[i]);
			if (e->event[i] && !IS_ERR(e->event[i]))
				acc += (__int128)diff * tracked_events[i].koeff;

			// print debug info about this event
			// pr_info("PID %d, Event %d: diff=%llu, coeff=%lld, partial_energy=%lld\n",
			// 	e->pid, i, diff, tracked_events[i].koeff,
			// 	(__int128)diff * tracked_events[i].koeff);
		}

		// If we have a timestamp delta, calculate power and update energy accordingly
		u64 delta = READ_ONCE(e->timestamp_delta);
		if (delta > 0) {
			u64 acc_64 = (s64)(acc >> 32) * 1000L; // milliwatt
			u64 delta_us = delta / 1000L;

			u64 p_old = atomic64_read(&e->power);
			s64 power = div64_s64(acc_64, delta_us ?: 1);
			u64 p_new = p_old - (p_old >> 3) + (power >> 3);
			atomic64_set(&e->power, p_new);

			// pr_info("PID %d: p_new=%llu mW, power=%lld, p_old=%llu\n",
			// 	e->pid, p_new, power, p_old);
		}

		atomic64_add((s64)(acc >> 32), &e->energy);
		WRITE_ONCE(e->energy_updated, true);

		// Release reference
		kref_put(&e->ref_count, release_traced_task);

		// pr_info("Estimated energy for PID %d: %llu\n", e->pid,
		// 	atomic64_read(&e->energy));

		spin_lock(&traced_tasks_lock);
	}
	spin_unlock(&traced_tasks_lock);

	if (atomic_read(&estimator_enabled))
		schedule_delayed_work(
			dwork, msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
}

static DECLARE_DELAYED_WORK(pacct_energy_estimate_work,
			    pacct_energy_estimate_workfn);

void pacct_start_energy_estimator(void)
{
	if (atomic_xchg(&estimator_enabled, 1))
		return;
	schedule_delayed_work(&pacct_energy_estimate_work,
			      msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
}

void pacct_stop_energy_estimator(void)
{
	atomic_set(&estimator_enabled, 0);
	cancel_delayed_work_sync(&pacct_energy_estimate_work);
}