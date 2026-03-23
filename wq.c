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

#include "pacct.h"

#define PACCT_SETUP_BUDGET 32
#define ENERGY_ESTIMATE_PERIOD_MS 30
#define TOTAL_POWER_GATHER_PERIOD_MS 150

extern struct list_head traced_tasks;
extern struct list_head retiring_traced_tasks;
extern spinlock_t traced_tasks_lock;
extern u64 last_pkg_raw, first_pkg_raw, last_ns;

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


//pick an element of traced_tasks, which is not set up yet
static bool pick_one_not_ready_candidate(struct traced_task **out)
{
	struct traced_task *e;

	*out = NULL;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry(e, &traced_tasks, list) {
		if (!READ_ONCE(e->ready)) {
			kref_get(&e->ref_count);
			*out = e;
			break;
		}
	}
	spin_unlock(&traced_tasks_lock);

	return *out != NULL;
}

//set the the non-ready tasks in traced_tasks
static void pacct_setup_workfn(struct work_struct *work)
{
	int done = 0;

	for (; done < PACCT_SETUP_BUDGET; done++) {
		struct traced_task *e;

		if (!pick_one_not_ready_candidate(&e))
			break;
		int ret = setup_traced_task(e);
		if (unlikely(ret != 0))
		{
			pr_alert("Failed to set up task. Trying again later");
		} else {
			WRITE_ONCE(e->ready,  1);
		}
		
		kref_put(&e->ref_count, release_traced_task);

		cond_resched();
	}
}

static DECLARE_WORK(pacct_setup_work, pacct_setup_workfn);

void queue_pacct_setup_work(void)
{
	queue_work(system_unbound_wq, &pacct_setup_work);
}

// ------- Retiring Tasks -------

// Delete entries from retired_traced_tasks list and clean them up if this was the last reference
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
		
		if (kref_put(&e->ref_count, release_traced_task)) {
			pr_warn("Traced task was not released during retiring: refcount != 0");
		}

		cond_resched(); // Why scheduling?
	}
}

static DECLARE_WORK(pacct_retire_work, pacct_retire_workfn);

void queue_pacct_retire_work(void)
{
	queue_work(system_unbound_wq, &pacct_retire_work);
}



static DECLARE_DELAYED_WORK(pacct_scan_tasks_work, pacct_scan_tasks_workfn);

void queue_pacct_scan_tasks(void)
{
	schedule_delayed_work(&pacct_scan_tasks_work, msecs_to_jiffies(100));
}

//--------- Estimate energy and power for traced tasks --------

// Estimate the energy from the counters via the model and calculate the power for a traced task
static __inline__ void pacct_estimate_traced_task_energy(struct traced_task *e)
{

	s64 energy_uj = 0;
	
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		struct perf_event *ev = READ_ONCE(e->event[i]);
		if (ev && !IS_ERR(ev)) {
			u64 counter = read_event_count_sleepable(ev);
			e->counts[i] = (s64) counter; // Buffer for proc filesystem
			
			// pr_info("PID %d(%s), Event %d: counter=%llu, coeff=%lld\n",
			// 	e->pid, e->comm, i, counter, tracked_events[i].coeff);
			
			// do energy_uj += (s64) counter * tracked_events[i].coeff with overflow checking
			if (counter > S64_MAX)
			pr_warn("casting overflow for event %d", i);
			s64 energy_single_uj;
			if (check_mul_overflow((s64)counter,
					       tracked_events[i].coeff,
					       &energy_single_uj)) {
				pr_warn("multiplication overflow for event %d",
					i);
			} else if (check_add_overflow(energy_uj,
						      energy_single_uj,
						      &energy_uj)) {
				pr_warn("addition overflow for event %d", i);
			}
		} else {
			pr_err("Failed to read counter: ev was error or null");
		}
	}
	energy_uj /= COUNTER_SCALE;

	// multiple by 28% to match the rapl value
	//energy_uj = (energy_uj * 9) >> 5;

	s64 old_energy_uj = e->energy_uj;
	e->energy_uj = energy_uj;
	if (e->better_timestamp_ns == 0) {
		e->power_mW = 0;
		e->better_timestamp_ns = ktime_get_ns();
		return;
	}
	s64 d_energy_uj = energy_uj - old_energy_uj;
	// if (d_energy_uj < 0) {
	// 	pr_info("Negative energy estimation, pid=%d(%s), energy_uj=%lld, old_energy_uj=%lld\n",
	// 		e->pid, e->comm, energy_uj, old_energy_uj);
	// }

	u64 now_ns = ktime_get_ns();
	u64 d_time_ns = now_ns - e->better_timestamp_ns;
	if (unlikely(d_time_ns == 0)) {
		pr_warn("zero time passed since last power calculation");
		return;
	}

	e->better_timestamp_ns = now_ns;
	s64 power_mW = div64_s64((d_energy_uj * 1000000LL), (s64)d_time_ns);

	// if (power_mW > 0)
	// 	pr_info("PID %d(%s): d_energy=%lld uJ, d_time=%llu ns, power=%lld mW\n",
	// 		e->pid, e->comm, d_energy_uj, d_time_ns, power_mW);
	e->power_mW = power_mW;
	return;
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

		spin_lock(&traced_tasks_lock);
	}
	spin_unlock(&traced_tasks_lock);

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
// Returns 0 on success, 1 on failure
static int sample_pkg_power(u64 *power_mW, u64 *energy_uj)
{
	// u64 raw = read_event_count(evt_cores);
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

	//pr_info("RAPL raw energy: %llu (uJ)\n", *energy_uj);

	*energy_uj = *energy_uj - first_pkg_raw; // Return energy since the first measurement for better comparison with our estimation
	if (first_pkg_raw == 0) { //First time this function was called
		first_pkg_raw = *energy_uj;
		last_pkg_raw = 0;
		*energy_uj = 0;
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
	struct traced_task *e;
	struct traced_task *n;

	//Summed values
	s64 sum_counter[PACCT_TRACED_EVENT_COUNT] = {0};
	s64 sum_energy = 0;
	s64 sum_power = 0;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(e, n, &traced_tasks, list) {
		kref_get(&e->ref_count);

		if (!READ_ONCE(e->ready)) {
			kref_put(&e->ref_count, release_traced_task);
			continue;
		}
		spin_unlock(&traced_tasks_lock);
		
		for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
			sum_counter[i] += e->counts[i];
		}
		sum_energy += e->energy_uj;
		sum_power += e->power_mW;

		kref_put(&e->ref_count, release_traced_task);
		spin_lock(&traced_tasks_lock);
	}
	spin_unlock(&traced_tasks_lock);

	u64 rapl_power_mW;  //measured using rapl in mW
	u64 rapl_energy_uj; //measured using rapl in uJ
	if (unlikely(sample_pkg_power(&rapl_power_mW, &rapl_energy_uj))) {
		rapl_power_mW = 0;
		rapl_energy_uj = 0;
	}

	// Update global_stats atomically using seqlock
	write_seqlock(&global_stats_lock);
	global_stats.energy = sum_energy;
	global_stats.power = sum_power;
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		global_stats.counter[i] = sum_counter[i];
	}
	global_stats.power_rapl = rapl_power_mW;
	global_stats.energy_rapl = rapl_energy_uj;
	write_sequnlock(&global_stats_lock);

	print_stats(&global_stats);

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

void pacct_start_energy_estimator(void)
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

void pacct_stop_energy_estimator(void)
{
	atomic_set(&estimator_enabled, 0);
	cancel_delayed_work_sync(&pacct_energy_estimate_work);
	cancel_delayed_work_sync(&pacct_gather_total_stats_work);
}