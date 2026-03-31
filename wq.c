#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/hashtable.h>
#include <linux/sched/signal.h>
#include <linux/perf_event.h>
#include <linux/overflow.h>

#include "traced-task.h"
#include "errors.h"
#include "model.h"
#include "wq.h"
#include "powercap.h"
#include "test-tracing.h"

#define PACCT_SETUP_BUDGET 32
#define ENERGY_ESTIMATE_PERIOD_MS 50
#define TOTAL_POWER_GATHER_PERIOD_MS 150

extern u64 last_pkg_raw, last_ns;

static atomic_t estimator_enabled = ATOMIC_INIT(0);

static bool enable_power_cap = 0;
module_param(enable_power_cap, bool, 0644);

// --- Setup tasks ------

// Add all existing processes to our traced_tasks list to be initialized later
static void pacct_scan_tasks_workfn(struct work_struct *work)
{
	struct task_struct *task;

	// Iterate over all existing tasks and add them to the traced_tasks list if
	// they are not kernel threads.
	for_each_process(task) {
		struct task_struct *ts = task;

		get_task_struct(ts);

		if (ts->flags & PF_KTHREAD) { //Ignore kernel
			put_task_struct(ts);
			continue;
		}

		{
			struct pacct_traced_task *e;
			int ret = pacct_traced_task_get_or_create(ts->pid, true, &e);
			if (ret) {
				pr_err("Failed to get or create traced task for PID %d\n",
				       ts->pid);
				put_task_struct(ts);
				continue;
			}

			kref_put(&e->ref_count, pacct_traced_task_release);
		}

		put_task_struct(ts);
	}

	pacct_queue_setup_work();
}

static DECLARE_DELAYED_WORK(pacct_scan_tasks_work, pacct_scan_tasks_workfn);

void pacct_queue_scan_tasks(void)
{
	schedule_delayed_work(&pacct_scan_tasks_work, msecs_to_jiffies(100));
}

/*
 * Picks an element of traced_tasks, which is not set up yet.
 * Increases refcount if found.
 */
static bool pick_one_not_ready_candidate(struct pacct_traced_task **out)
{
	struct pacct_traced_task *e;

	*out = NULL;

	spin_lock(&pacct_traced_tasks_lock);
	list_for_each_entry(e, &pacct_traced_tasks, list) {
		if (!READ_ONCE(e->ready)) {
			kref_get(&e->ref_count);
			*out = e;
			break;
		}
	}
	spin_unlock(&pacct_traced_tasks_lock);

	return *out != NULL;
}

//set the the non-ready tasks in traced_tasks
static void pacct_setup_workfn(struct work_struct *work)
{
	int done = 0;

	// TODO For the first time we do this, shouldn't we do all processes?
	// TODO Not just PACCT_SETUP_BUDGET many?
	for (; done < PACCT_SETUP_BUDGET; done++) {
		struct pacct_traced_task *e;

		if (!pick_one_not_ready_candidate(&e))
			break;
		int ret = pacct_traced_task_setup(e);
		if (unlikely(ret != 0)) {
			pr_alert("Failed to set up task. Trying again later");
		}

		kref_put(&e->ref_count, pacct_traced_task_release);

		cond_resched();
	}
}

static DECLARE_WORK(pacct_setup_work, pacct_setup_workfn);

void pacct_queue_setup_work(void)
{
	queue_work(system_unbound_wq, &pacct_setup_work);
}

// ------- Retiring Tasks -------

// Delete entries from retired_traced_tasks list and clean them up if this was the last reference
static void pacct_retire_workfn(struct work_struct *work)
{
	struct pacct_traced_task *e;

	for (;;) {
		spin_lock(&pacct_traced_tasks_lock);

		if (list_empty(&pacct_retiring_traced_tasks)) {
			spin_unlock(&pacct_traced_tasks_lock);
			break;
		}

		e = list_first_entry(&pacct_retiring_traced_tasks,
				     struct pacct_traced_task, retire_node);
		list_del_init(&e->retire_node);
		spin_unlock(&pacct_traced_tasks_lock);

		if (kref_put(&e->ref_count, pacct_traced_task_release)) {
			pr_warn("Traced task was not released during retiring: refcount != 0");
		}

		cond_resched();
	}
}

static DECLARE_WORK(pacct_retire_work, pacct_retire_workfn);

void pacct_queue_retire_work(void)
{
	queue_work(system_unbound_wq, &pacct_retire_work);
}

//--------- Estimate energy and power for traced tasks --------

static void pacct_estimate_traced_task_energy(struct pacct_traced_task *e)
{
	u64 energy_uj = 0;
	u64 power_wallclock_mW = 0;
	u64 power_cpu_mW = 0;

	unsigned long flags;
	// TODO not sure if this is the spinlock variant we want.
	// But this one should always work.
	spin_lock_irqsave(&e->periodic_lock, flags);
	for (int i = 0; i < PACCT_TEST_TRACING_NUM; i++) {
		if (pacct_test_traces[i].tt == e) {
			pr_info("test-task %d's periodic_data {\n", i);
			if (e->periodic_data.time_efficiency_ns +
				    e->periodic_data.time_performance_ns ==
			    0) {
				pr_info(" <runtime 0>\n");
			} else {
				pr_info("  elapsed time (ns): %llu\n",
					ktime_get_ns() -
						e->periodic_data.time_start_ns);
				pr_info("  time on effi (ns): %llu\n",
					e->periodic_data.time_efficiency_ns);
				pr_info("  time on perf (ns): %llu\n",
					e->periodic_data.time_performance_ns);
				// TODO hard-coded
				pr_info("  counter diff effi: %llu %llu %llu %llu\n",
					e->periodic_data
						.counter_diff_efficiency[0],
					e->periodic_data
						.counter_diff_efficiency[1],
					e->periodic_data
						.counter_diff_efficiency[2],
					e->periodic_data
						.counter_diff_efficiency[3]);
				pr_info("  counter diff perf: %llu %llu %llu %llu %llu %llu %llu %llu\n",
					e->periodic_data
						.counter_diff_performance[0],
					e->periodic_data
						.counter_diff_performance[1],
					e->periodic_data
						.counter_diff_performance[2],
					e->periodic_data
						.counter_diff_performance[3],
					e->periodic_data
						.counter_diff_performance[4],
					e->periodic_data
						.counter_diff_performance[5],
					e->periodic_data
						.counter_diff_performance[6],
					e->periodic_data
						.counter_diff_performance[7]);
			}
			pr_info("}\n");
		}
	}
	int res = pacct_model_eval(&e->periodic_data, &energy_uj,
				   &power_wallclock_mW, &power_cpu_mW);
	(void)res; // no need for error handling here
	for (int i = 0; i < PACCT_TEST_TRACING_NUM; i++) {
		if (pacct_test_traces[i].tt == e) {
			if (res == 0) {
				pr_info("--- energy_uj: %llu\n", energy_uj);
				pr_info("--- power_wallclock_mW: %llu\n",
					power_wallclock_mW);
				pr_info("--- power_cpu_mW: %llu\n",
					power_cpu_mW);
			} else {
				pr_info("--- model failed\n");
			}
		}
	}

	// energy is accumulated, power is just the value itself
	e->energy_uj += energy_uj;
	e->power_wallclock_mW = power_wallclock_mW;
	e->power_cpu_mW = power_cpu_mW;

	// reset for next period
	memset(&e->periodic_data, 0, sizeof(e->periodic_data));
	e->periodic_data.time_start_ns = ktime_get_ns();

	spin_unlock_irqrestore(&e->periodic_lock, flags);
}

static void pacct_energy_estimate_workfn(struct work_struct *work)
{
	struct delayed_work *dwork =
		container_of(work, struct delayed_work, work);

	struct pacct_traced_task *e;

	spin_lock(&pacct_traced_tasks_lock);
	list_for_each_entry(e, &pacct_traced_tasks, list) {
		kref_get(&e->ref_count);

		// NOTE: removed ready check here because it's not necessary anymore
		if (READ_ONCE(e->retiring)) {
			kref_put(&e->ref_count, pacct_traced_task_release);
			continue;
		}

		spin_unlock(&pacct_traced_tasks_lock);

		pacct_estimate_traced_task_energy(e);

		kref_put(&e->ref_count, pacct_traced_task_release);
		// TODO isn't this a use-after-free here? We potentially free
		// the entry e, but still use it to advance in the list

		spin_lock(&pacct_traced_tasks_lock);
	}
	spin_unlock(&pacct_traced_tasks_lock);

	if (atomic_read(&estimator_enabled))
		schedule_delayed_work(
			dwork, msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
}

static DECLARE_DELAYED_WORK(pacct_energy_estimate_work,
			    pacct_energy_estimate_workfn);

// ------- Gather stats of all traced tasks and compare with rapl -------

static u32 rapl_eu_shift; // energy unit shift

static int rapl_read_eu_shift_on_cpu(int cpu)
{
	u64 v;
	int ret = rdmsrq_safe_on_cpu(cpu, MSR_RAPL_POWER_UNIT, &v);
	if (ret)
		return ret;
	rapl_eu_shift = (v >> 8) & 0x1f;
	return 0;
}

static int rapl_read_pkg_energy_uj_on_cpu(int cpu, u64 *uj)
{
	u64 raw64;
	int ret = rdmsrq_safe_on_cpu(cpu, MSR_PKG_ENERGY_STATUS, &raw64);
	if (ret)
		return ret;

	// the energy status is a 32-bit value that wraps around, so we only care about the lower 32 bits
	u32 raw = (u32)raw64;

	// *1e6 -> uJ, >> rapl_eu_shift to convert to actual energy value based on the energy unit shift
	__uint128_t tmp = (__uint128_t)raw * 1000000ULL;
	tmp >>= rapl_eu_shift;

	*uj = (u64)tmp;
	return 0;
}

//Calculate the power and energy measured via rapl
static int sample_pkg_power(u64 *power_mW, u64 *energy_uj)
{
	u64 now = ktime_get_ns();

	if (rapl_eu_shift == 0) {
		int ret = rapl_read_eu_shift_on_cpu(0);
		if (ret) {
			pr_err("Failed to read RAPL energy unit shift: %d\n",
			       ret);
			return 1;
		}
		pr_info("RAPL energy unit shift: %u\n", rapl_eu_shift);
	}

	int ret = rapl_read_pkg_energy_uj_on_cpu(0, energy_uj);
	if (ret) {
		pr_err("Failed to read RAPL energy on CPU 0: %d\n", ret);
		return 1;
	}

	pr_info("RAPL raw energy: %llu (uJ)\n", *energy_uj);

	if (last_pkg_raw == 0) { //First time this function was called
		last_pkg_raw = *energy_uj;
		last_ns = now;
		return 1;
	}

	u64 d_raw = *energy_uj - last_pkg_raw;
	u64 dt_ns = now - last_ns;

	last_pkg_raw = *energy_uj;
	last_ns = now;

	if (unlikely(dt_ns == 0))
		return 1;

	// Power in mW = energy in uJ / time in ms = energy in uJ / time in ns * 1e6
	*power_mW = div64_u64(d_raw * 1000000, dt_ns);
	return 0;
}

static void pacct_gather_total_stats_workfn(struct work_struct *work)
{
	struct delayed_work *dwork =
		container_of(work, struct delayed_work, work);
	struct pacct_traced_task *e;
	struct pacct_traced_task *n;

	//Summed values
	s64 sum_energy_uj = 0;
	s64 sum_power_wallclock_mW = 0;

	spin_lock(&pacct_traced_tasks_lock);
	list_for_each_entry_safe(e, n, &pacct_traced_tasks, list) {
		kref_get(&e->ref_count);

		if (!READ_ONCE(e->ready)) {
			kref_put(&e->ref_count, pacct_traced_task_release);
			continue;
		}
		spin_unlock(&pacct_traced_tasks_lock);

		sum_energy_uj += e->energy_uj;
		sum_power_wallclock_mW += e->power_wallclock_mW;

		kref_put(&e->ref_count, pacct_traced_task_release);
		spin_lock(&pacct_traced_tasks_lock);
	}
	spin_unlock(&pacct_traced_tasks_lock);

	global_stats.energy = sum_energy_uj;
	global_stats.power = sum_power_wallclock_mW;

	u64 rapl_power_mW; //measured using rapl in mW
	u64 rapl_energy_uj; //measured using rapl in uJ
	if (!sample_pkg_power(&rapl_power_mW, &rapl_energy_uj)) {
		global_stats.power_rapl = rapl_power_mW;
		global_stats.energy_rapl = rapl_energy_uj;
		pr_info("Power: estimated power (wallclock): %lld mW, pkg power: %llu mW\n",
			sum_power_wallclock_mW, rapl_power_mW);
	}

	// simple power capping control based on the sampled package power
	if (enable_power_cap)
		pacct_powercap_control_step(rapl_power_mW);

	if (atomic_read(&estimator_enabled))
		schedule_delayed_work(
			dwork, msecs_to_jiffies(TOTAL_POWER_GATHER_PERIOD_MS));
}

static DECLARE_DELAYED_WORK(pacct_gather_total_stats_work,
			    pacct_gather_total_stats_workfn);

//--------- Start or stop periodic tasks -----------

void pacct_queue_energy_estimator_start(void)
{
	if (atomic_xchg(&estimator_enabled,
			1)) //Ensure estimator is only activated once
		return;

	schedule_delayed_work(&pacct_energy_estimate_work,
			      msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
	// Sum power of all processes and compare to rapl printing to log
	schedule_delayed_work(&pacct_gather_total_stats_work,
			      msecs_to_jiffies(TOTAL_POWER_GATHER_PERIOD_MS));
}

void pacct_queue_energy_estimator_stop(void)
{
	atomic_set(&estimator_enabled, 0);
	cancel_delayed_work_sync(&pacct_energy_estimate_work);
	cancel_delayed_work_sync(&pacct_gather_total_stats_work);
}
