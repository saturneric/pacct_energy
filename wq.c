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
#define ENERGY_ESTIMATE_PERIOD_MS 30
#define TOTAL_POWER_GATHER_PERIOD_MS 150

extern struct list_head traced_tasks;
extern struct list_head retiring_traced_tasks;
extern spinlock_t traced_tasks_lock;
extern u64 total_power;
extern struct perf_event *evt_pkg, *evt_cores;
extern u64 last_pkg_raw, last_ns;

static atomic_t estimator_enabled = ATOMIC_INIT(0);

static bool enable_power_cap = 0;
module_param(enable_power_cap, bool, 0644);

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

// Estimate the energy from the counters via the model and calculate the power for each traced task
static __inline__ void pacct_estimate_traced_task_energy(struct traced_task *e)
{
	u64 diff_count[PACCT_TRACED_EVENT_COUNT];
	u64 ts_delta_ns;
	u64 wall_ts_delta_ns;

	// Atomically read and reset the diff_counts and delta_timestamp_acc for this
	// task. We can get a slightly stale value here, but that's acceptable for
	// energy estimation, and it can help us avoid contention with the energy
	// estimation work that might be updating these values at the same time.
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		diff_count[i] = atomic64_xchg(&e->diff_counts[i], 0);
	}
	ts_delta_ns = atomic64_xchg(&e->delta_exec_runtime_acc, 0);
	wall_ts_delta_ns = atomic64_xchg(&e->delta_timestamp_acc, 0);
	e->total_exec_runtime_acc += ts_delta_ns;

	// Calculate energy estimation based on diff_counts and coefficients
	s64 acc = 0;
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		u64 diff = READ_ONCE(diff_count[i]);
		if (e->event[i] && !IS_ERR(e->event[i]))
			acc += diff * tracked_events[i].koeff;

		// print debug info about this event
		// pr_info("PID %d, Event %d: diff=%llu, coeff=%lld, partial_energy=%lld\n",
		// 	e->pid, i, diff, tracked_events[i].koeff,
		// 	(__int128)diff * tracked_events[i].koeff);
	}

	// We might get some negative energy estimation due to noise, but we can just
	// treat it as zero in that case since negative energy doesn't make sense.
	if (acc < 0) {
		pr_info("Encountered negative energy estimation.");
		acc = 0;
	}

	atomic64_add(acc, &e->energy); // uJ

	// Calculate power estimation based on energy and time delta
	u64 energy = atomic64_read(&e->energy);
	u64 total_exec_runtime_us =
		e->total_exec_runtime_acc / 1000; // Convert ns to us
	// To avoid division by zero, we can use the current timestamp delta as an
	// approximation of the time delta if total_exec_runtime_acc is still zero
	u64 power = div64_u64(
		energy * 1000,
		total_exec_runtime_us ? // nJ / us = 10^-9J/ 10^-6s= 1mW
			total_exec_runtime_us :
			1);

	atomic64_set(&e->power_a, power);

	// Calculate power instance based on energy delta and execution runtime delta
	s64 dE_uJ = acc;
	if (dE_uJ < 0)
		dE_uJ = 0;

	u64 dt_us = ts_delta_ns / 1000;
	if (dt_us == 0)
		dt_us = 1;
	u64 power_i = div64_u64((u64)dE_uJ * 1000, dt_us);
	u64 old = atomic64_read(&e->power_i);
	// smoothing to reduce noise 75% old value + 25% new value
	u64 smoothed = (old * 3 + power_i) >> 2;

	// We can get some 0 energy delta due to estimation noise
	if (dE_uJ != 0) {
		atomic64_set(&e->power_i, smoothed);
	}

	// Calculate power based on wall clock time delta
	dt_us = wall_ts_delta_ns / 1000;
	if (dt_us == 0)
		dt_us = 1;
	u64 power_w = div64_u64((u64)dE_uJ * 1000, dt_us);
	old = atomic64_read(&e->power_w);
	// smoothing to reduce noise 75% old value + 25% new value
	smoothed = (old * 3 + power_w) >> 2;

	// We can get some 0 energy delta due to estimation noise
	if (dE_uJ != 0) {
		atomic64_set(&e->power_w, smoothed);
	}

	// 100W threshold for high power task - this can help us identify any
	// abnormally high power tasks which might indicate an issue with our
	// estimation or a real power hog
	// if (unlikely(power > 100000)) {
	// 	pr_warn("[!!!!!]High power task: PID %d, energy acc=%lld, energy=%llu uJ, total_exec_runtime_us=%llu, power=%llu mW\n",
	// 		e->pid, (s64)(acc >> 32), energy, total_exec_runtime_us,
	// 		power);
	// 	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
	// 		pr_warn("[!!!!!]Event %d: diff=%llu, coeff=%lld\n", i,
	// 			diff_count[i], tracked_events[i].koeff);
	// 	}
	// }
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

// Add all existing processes to our traced_tasks list to be initialized later
static void pacct_scan_tasks_workfn(struct work_struct *work)
{
	struct task_struct *task;

	// Iterate over all existing tasks and add them to the traced_tasks list if
	// they are not kernel threads.
	for_each_process(task) {
		struct task_struct *ts = task;

		get_task_struct(ts);

		if (ts->flags & PF_KTHREAD) {
			put_task_struct(ts);
			continue;
		}

		{
			struct traced_task *e = get_or_create_traced_task(
				ts->pid, ts->comm, true);
			if (!e) {
				pr_err("Failed to get or create traced task for PID %d\n",
				       ts->pid);
				put_task_struct(ts);
				continue;
			}

			// pr_info("Initially tracing existing process: PID %d, COMM %s\n",
			// 	ts->pid, ts->comm);

			kref_put(&e->ref_count, release_traced_task);
		}

		put_task_struct(ts);
	}

	queue_pacct_setup_work();
}

static DECLARE_DELAYED_WORK(pacct_scan_tasks_work, pacct_scan_tasks_workfn);

void queue_pacct_scan_tasks(void)
{
	schedule_delayed_work(&pacct_scan_tasks_work, msecs_to_jiffies(100));
}

//Calculate the power measured via rapl
static u64 sample_pkg_power(void)
{
	// u64 raw = read_event_count(evt_cores);
	u64 now = ktime_get_ns();

	u64 raw = 0;
	int ret = rapl_read_pkg_energy_uj_on_cpu(0, &raw);
	if (ret) {
		pr_err("Failed to read RAPL energy on CPU 0: %d\n", ret);
		return 0;
	}

	pr_info("RAPL raw energy: %llu (uJ)\n", raw);

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

		u64 pw = atomic64_read(&e->power_w);
		total_power += pw;

		// struct task_struct *ts = get_task_by_pid(e->pid);
		// if (ts) {
		// 	int cpu = task_cpu(ts);

		// 	// only print tasks running on P-core
		// 	if (cpu >= 0 && cpu < 12)
		// 		pr_info("PID %d (%s): energy=%llu uJ, power=%llu mW, "
		// 			"power_i=%llu mW, power_w=%llu mW record_count=%d, "
		// 			"cpu=%d\n",
		// 			e->pid, e->comm,
		// 			atomic64_read(&e->energy),
		// 			atomic64_read(&e->power_a),
		// 			atomic64_read(&e->power_i),
		// 			atomic64_read(&e->power_w),
		// 			atomic_read(&e->record_count),
		// 			task_cpu(ts));

		// 	put_task_struct(ts);
		// }

		kref_put(&e->ref_count, release_traced_task);
		spin_lock(&traced_tasks_lock);
	}
	spin_unlock(&traced_tasks_lock);

	u64 pkg_power = sample_pkg_power(); //measured using rapl
	pr_info("Power: avg power: %llu mW, pkg power: %llu mW\n", total_power,
		pkg_power);

	// simple power capping control based on the sampled package power
	if (enable_power_cap)
		pacct_powercap_control_step(pkg_power);

	if (atomic_read(&estimator_enabled))
		schedule_delayed_work(
			dwork, msecs_to_jiffies(TOTAL_POWER_GATHER_PERIOD_MS));
}

static DECLARE_DELAYED_WORK(pacct_gather_total_power_work,
			    pacct_gather_total_power_workfn);

void pacct_start_energy_estimator(void)
{
	if (atomic_xchg(&estimator_enabled,
			1)) //Ensure estimator is only activated once
		return;

	schedule_delayed_work(&pacct_energy_estimate_work,
			      msecs_to_jiffies(ENERGY_ESTIMATE_PERIOD_MS));
	// Sum power of all processes and compare to rapl printing to log
	schedule_delayed_work(&pacct_gather_total_power_work,
			      msecs_to_jiffies(TOTAL_POWER_GATHER_PERIOD_MS));
}

void pacct_stop_energy_estimator(void)
{
	atomic_set(&estimator_enabled, 0);
	cancel_delayed_work_sync(&pacct_energy_estimate_work);
	cancel_delayed_work_sync(&pacct_gather_total_power_work);
}