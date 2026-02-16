#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/hashtable.h>
#include <linux/sched/signal.h>
#include <linux/perf_event.h>

#include "pacct.h"

#define PACCT_SETUP_BUDGET 32
#define ENERGY_ESTIMATE_PERIOD_MS 1
#define TOTAL_POWER_GATHER_PERIOD_MS 150

extern struct list_head traced_tasks;
extern struct list_head retiring_traced_tasks;
extern spinlock_t traced_tasks_lock;
extern u64 total_power;
extern struct perf_event *evt_pkg, *evt_cores;
extern u64 last_pkg_raw, last_ns;

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

static __inline__ void pacct_estimate_traced_task_energy(struct traced_task *e)
{
	u64 diff_count[PACCT_TRACED_EVENT_COUNT];
	u64 ts_delta_ns;

	// Atomically read and reset the diff_counts and delta_timestamp_acc for this
	// task. We can get a slightly stale value here, but that's acceptable for
	// energy estimation, and it can help us avoid contention with the energy
	// estimation work that might be updating these values at the same time.
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		diff_count[i] = atomic64_xchg(&e->diff_counts[i], 0);
	}
	ts_delta_ns = atomic64_xchg(&e->delta_timestamp_acc, 0);
	e->total_exec_runtime_acc += ts_delta_ns;

	// Calculate energy estimation based on diff_counts and coefficients
	__int128 acc = 0;
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		u64 diff = READ_ONCE(diff_count[i]);
		if (e->event[i] && !IS_ERR(e->event[i]))
			acc += (__int128)diff * tracked_events[i].koeff;

		// print debug info about this event
		// pr_info("PID %d, Event %d: diff=%llu, coeff=%lld, partial_energy=%lld\n",
		// 	e->pid, i, diff, tracked_events[i].koeff,
		// 	(__int128)diff * tracked_events[i].koeff);
	}

	atomic64_add((s64)(acc >> 32), &e->energy); // uJ

	// Calculate power estimation based on energy and time delta
	u64 energy = atomic64_read(&e->energy);
	u64 total_exec_runtime_us = e->total_exec_runtime_acc / 1000;
	// To avoid division by zero, we can use the current timestamp delta as an
	// approximation of the time delta if total_exec_runtime_acc is still zero
	u64 power = div64_u64(energy * 1000, total_exec_runtime_us ?
						     total_exec_runtime_us :
						     1);
	atomic64_set(&e->power, power);
}

static void pacct_energy_estimate_workfn(struct work_struct *work)
{
	struct delayed_work *dwork =
		container_of(work, struct delayed_work, work);

	struct traced_task *e, *n;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(e, n, &traced_tasks, list) {
		kref_get(&e->ref_count);

		if (!READ_ONCE(e->ready) || READ_ONCE(e->retiring)) {
			kref_put(&e->ref_count, release_traced_task);
			continue;
		}

		spin_unlock(&traced_tasks_lock);

		pacct_estimate_traced_task_energy(e);

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

static void paact_scan_tasks_workfn(struct work_struct *work)
{
	struct task_struct *task;

	for_each_process(task) {
		// Don't trace kernel threads
		if (task->flags & PF_KTHREAD)
			continue;

		// For each existing task, create a traced_task entry for it and mark it as needing setup
		struct traced_task *e =
			get_or_create_traced_task(task->pid, true);
		if (!e) {
			pr_err("Failed to get or create traced task for PID %d\n",
			       task->pid);
			continue;
		}

		pr_info("Initially tracing existing process: PID %d, COMM %s\n",
			task->pid, task->comm);

		kref_put(&e->ref_count, release_traced_task);
	}
}

static DECLARE_DELAYED_WORK(paact_scan_tasks_work, paact_scan_tasks_workfn);

void queue_paact_scan_tasks(void)
{
	schedule_delayed_work(&paact_scan_tasks_work, msecs_to_jiffies(100));
}

static u64 sample_pkg_power(void)
{
	u64 raw = read_event_count(evt_pkg);
	u64 now = ktime_get_ns();

	if (last_pkg_raw == 0) {
		last_pkg_raw = raw;
		last_ns = now;
		return 0;
	}

	u64 d_raw = raw - last_pkg_raw;
	u64 dt_ns = now - last_ns;

	last_pkg_raw = raw;
	last_ns = now;

	if (unlikely(dt_ns == 0))
		return 0;

	u64 numerator = mul_u64_u64_div_u64(d_raw, 1000000000000ULL, dt_ns);
	return numerator >> 32;
}

static void pacct_gather_total_power_workfn(struct work_struct *work)
{
	struct delayed_work *dwork =
		container_of(work, struct delayed_work, work);
	struct traced_task *e;
	struct traced_task *n;

	WRITE_ONCE(total_power, 0);

	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(e, n, &traced_tasks, list) {
		kref_get(&e->ref_count);

		if (!READ_ONCE(e->ready)) {
			kref_put(&e->ref_count, release_traced_task);
			continue;
		}

		spin_unlock(&traced_tasks_lock);

		u64 power = atomic64_read(&e->power);
		total_power += power;

		// print abnormally high power tasks for debugging
		if (power > 100000) { // 100W threshold for high power task
			pr_info("[!!!!!]High power task: PID %d, Power %llu mW\n",
				e->pid, power);
		}

		kref_put(&e->ref_count, release_traced_task);
		spin_lock(&traced_tasks_lock);
	}
	spin_unlock(&traced_tasks_lock);

	u64 pkg_power = sample_pkg_power();
	pr_info("Total estimated power: %llu mW, pkg power: %llu mW\n",
		total_power, pkg_power);

	if (atomic_read(&estimator_enabled))
		schedule_delayed_work(
			dwork, msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
}

static DECLARE_DELAYED_WORK(paact_gather_total_power_work,
			    pacct_gather_total_power_workfn);

void pacct_start_energy_estimator(void)
{
	if (atomic_xchg(&estimator_enabled, 1))
		return;

	schedule_delayed_work(&pacct_energy_estimate_work,
			      msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
	schedule_delayed_work(&paact_gather_total_power_work,
			      msecs_to_jiffies(TOTAL_POWER_GATHER_PERIOD_MS));
}

void pacct_stop_energy_estimator(void)
{
	atomic_set(&estimator_enabled, 0);
	cancel_delayed_work_sync(&pacct_energy_estimate_work);
	cancel_delayed_work_sync(&paact_gather_total_power_work);
}