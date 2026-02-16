#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/tracepoint.h>
#include <linux/smp.h>

#include "pacct.h"

MODULE_AUTHOR("pm3");
MODULE_DESCRIPTION("Process Energy Accounting Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

// Tracepoint for events
static struct tracepoint *tp_sched_switch;
static struct tracepoint *tp_sched_exit;
static struct tracepoint *tp_sched_fork;

// List of tasks being traced
struct list_head traced_tasks;
// List of tasks that are being retired (for cleanup)
struct list_head retiring_traced_tasks;
// Lock to protect access to the traced_tasks list
spinlock_t traced_tasks_lock;

// Global variable to hold the total estimated power consumption across all traced tasks
u64 total_power; // average power in mW (based on wall clock time)

// Perf event for reading package-level energy consumption
static int rapl_pmu_type = 32;
module_param(rapl_pmu_type, int, 0444);
struct perf_event *evt_pkg, *evt_cores;
u64 last_pkg_raw, last_ns;

#define RAPL_EVT_CORES 0x1
#define RAPL_EVT_PKG 0x2

static struct traced_task *get_traced_task(pid_t pid)
{
	return get_or_create_traced_task(pid, NULL, false);
}

static __inline__ u64 u64_delta_sat(u64 now, u64 prev)
{
	return (now >= prev) ? (now - prev) : 0;
}

static __inline__ void init_traced_task(struct traced_task *e, u64 exec_runtime)
{
	// This can happen at init time because we set last_exec_runtime to 0 initially
	// and only update it after the first switch.
	WRITE_ONCE(e->last_exec_runtime, exec_runtime);
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		struct perf_event *ev = READ_ONCE(e->event[i]);
		if (ev && !IS_ERR(ev))
			WRITE_ONCE(e->counts[i], read_event_count(ev));
	}

	// Also set the last timestamp to now to avoid having a large delta at the first estimation
	u64 now = ktime_get_ns();
	atomic64_set(&e->last_timestamp_ns, now);
	return;
}

static void record_task_event_counts(struct traced_task *e,
				     struct task_struct *ts)
{
	atomic_inc(&e->record_count);

	// Update the timestamp and calculate the delta since the last switch
	u64 exec_runtime = READ_ONCE(ts->se.sum_exec_runtime);
	u64 last_exec_runtime = READ_ONCE(e->last_exec_runtime);
	if (last_exec_runtime == 0) {
		// This can happen if the task is scheduled before we get a chance to initialize it
		init_traced_task(e, exec_runtime);
		return;
	}

	u64 delta = u64_delta_sat(exec_runtime, last_exec_runtime);
	WRITE_ONCE(e->last_exec_runtime, exec_runtime);
	atomic64_add(delta, &e->delta_exec_runtime_acc);

	u64 now = ktime_get_ns();
	u64 last_timestamp = atomic64_read(&e->last_timestamp_ns);
	if (unlikely(last_timestamp == 0)) {
		// This can happen if the task is scheduled before we get a chance to initialize it
		init_traced_task(e, exec_runtime);
		return;
	}

	delta = u64_delta_sat(now, last_timestamp);
	atomic64_set(&e->last_timestamp_ns, now);
	atomic64_add(delta, &e->delta_timestamp_acc);

	// For each event, read the current count, calculate the diff since last time,
	// and accumulate the diff
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		struct perf_event *ev = READ_ONCE(e->event[i]);
		if (ev && !IS_ERR(ev)) {
			u64 val = read_event_count(ev); // new value
			u64 diff = u64_delta_sat(val, READ_ONCE(e->counts[i]));

			atomic64_add(diff, &e->diff_counts[i]);
			WRITE_ONCE(e->counts[i], val);
		}
	}
}

static void pacct_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	struct traced_task *e = get_traced_task(prev->pid);
	if (!e)
		return;

	if (!READ_ONCE(e->ready)) {
		WRITE_ONCE(e->needs_setup, true);
		goto out;
	}

	record_task_event_counts(e, prev);

out:
	kref_put(&e->ref_count, release_traced_task);
}

static void pacct_process_fork(void *ignore, struct task_struct *parent,
			       struct task_struct *child)
{
	// Don't trace kernel threads
	if (child->flags & PF_KTHREAD)
		return;

	struct traced_task *e =
		get_or_create_traced_task(child->pid, child->comm, true);
	if (!e) {
		pr_err("Failed to get or create traced task for PID %d\n",
		       child->pid);
		return;
	}

	// pr_info("Start to trace new process: PID %d, COMM %s\n", child->pid,
	// 	child->comm);

	// schedule setup work for the new task to initialize its perf events
	queue_pacct_setup_work();

	kref_put(&e->ref_count, release_traced_task);
}

static void pacct_process_exit(void *ignore, struct task_struct *p)
{
	struct traced_task *e = get_traced_task(p->pid);
	if (!e)
		return;

	// Record final event counts for this exiting task before we clean it up.
	record_task_event_counts(e, p);

	// Mark this task as retiring so that the sample_workfn can skip it if it hasn't run yet
	WRITE_ONCE(e->retiring, true);

	// remove from traced_tasks
	spin_lock(&traced_tasks_lock);
	list_del_init(&e->list);
	spin_unlock(&traced_tasks_lock);

	// // print debug info about the exiting task
	// pr_info("Process exiting: PID %d, COMM \"%s\", energy estimate %llu (uJ), power estimate %llu (mW), exec_runtime=%llu\n",
	// 	e->pid, p->comm, atomic64_read(&e->energy),
	// 	atomic64_read(&e->power_a), p->se.sum_exec_runtime);

	// // If energy is not zero, print the final event counts and diffs for this task
	// // This can help us understand the event activity of the task.
	// if (atomic64_read(&e->energy) != 0) {
	// 	pr_info("Event counts for exiting PID %d:\n", e->pid);
	// 	// print each event's final count and diff for this exiting task
	// 	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
	// 		pr_info("[%d]: count=%llu, diff=%llu\n", i,
	// 			e->counts[i],
	// 			atomic64_read(&e->diff_counts[i]));
	// 	}
	// }

	// add to retiring_traced_tasks for cleanup
	list_add_tail(&e->retire_node, &retiring_traced_tasks);

	// we'd got a ref from get_traced_task()
	kref_put(&e->ref_count, release_traced_task);
}

static void tp_lookup_cb(struct tracepoint *tp, void *priv)
{
	const char *name = priv;

	if (!strcmp(tp->name, name)) {
		if (!strcmp(name, "sched_switch"))
			tp_sched_switch = tp;
		else if (!strcmp(name, "sched_process_exit"))
			tp_sched_exit = tp;
		else if (!strcmp(name, "sched_process_fork"))
			tp_sched_fork = tp;
	}
}

static struct perf_event *open_rapl_event(u64 event_code)
{
	struct perf_event_attr attr = {
		.type = rapl_pmu_type,
		.config = event_code,
	};

	attr.disabled = 1;
	attr.size = sizeof(attr);

	int cpu = cpumask_first(cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return ERR_PTR(-ENODEV);

	struct perf_event *ev =
		perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
	if (IS_ERR(ev)) {
		pr_err("perf_event_create_kernel_counter failed for event code 0x%02llx: %ld\n",
		       event_code, PTR_ERR(ev));
		return ev;
	}

	perf_event_enable(ev);
	return ev;
}

static int rapl_mod_init(void)
{
	evt_pkg = open_rapl_event(RAPL_EVT_PKG);
	evt_cores = open_rapl_event(RAPL_EVT_CORES);

	if (IS_ERR(evt_pkg) || IS_ERR(evt_cores)) {
		pr_err("open rapl events failed: pkg=%ld cores=%ld\n",
		       IS_ERR(evt_pkg) ? PTR_ERR(evt_pkg) : 0L,
		       IS_ERR(evt_cores) ? PTR_ERR(evt_cores) : 0L);

		if (!IS_ERR(evt_pkg))
			perf_event_release_kernel(evt_pkg);
		if (!IS_ERR(evt_cores))
			perf_event_release_kernel(evt_cores);
		return -EINVAL;
	}

	pr_info("RAPL events ready (type=%d): pkg/cores\n", rapl_pmu_type);
	return 0;
}

static void rapl_mod_exit(void)
{
	if (evt_pkg && !IS_ERR(evt_pkg))
		perf_event_release_kernel(evt_pkg);
	if (evt_cores && !IS_ERR(evt_cores))
		perf_event_release_kernel(evt_cores);
}

static void clean_traced_task(void)
{
	// Move all currently traced tasks to the retiring list for cleanup
	struct traced_task *entry, *tmp;
	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(entry, tmp, &traced_tasks, list) {
		list_del_init(&entry->list);
		list_add_tail(&entry->retire_node, &retiring_traced_tasks);
	}
	spin_unlock(&traced_tasks_lock);
}

static int __init pacct_energy_init(void)
{
	int ret;

	pr_info("pacct_energy init\n");

	// Initialize the list of traced tasks and the lock
	spin_lock_init(&traced_tasks_lock);
	INIT_LIST_HEAD(&traced_tasks);
	INIT_LIST_HEAD(&retiring_traced_tasks);

	// Initialize the powercap interfaces and get the initial CPU frequency caps
	ret = powercap_init_caps();
	if (ret) {
		pr_err("powercap init failed: %d\n", ret);
		goto err;
	}

	for_each_kernel_tracepoint(tp_lookup_cb, "sched_switch");
	if (!tp_sched_switch) {
		pr_err("tracepoint sched_switch not found\n");
		ret = -ENOENT;
		goto err;
	}

	for_each_kernel_tracepoint(tp_lookup_cb, "sched_process_fork");
	if (!tp_sched_fork) {
		pr_err("tracepoint sched_process_fork not found\n");
		ret = -ENOENT;
		goto err;
	}

	for_each_kernel_tracepoint(tp_lookup_cb, "sched_process_exit");
	if (!tp_sched_exit) {
		pr_err("tracepoint sched_process_exit not found\n");
		ret = -ENOENT;
		goto err;
	}

	// Register the probe function for the sched_switch tracepoint
	ret = tracepoint_probe_register(tp_sched_switch,
					(void *)pacct_sched_switch, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register failed: %d\n", ret);
		goto err;
	}

	ret = tracepoint_probe_register(tp_sched_fork,
					(void *)pacct_process_fork, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register for fork failed: %d\n", ret);
		goto err_tp_sched_switch;
	}

	ret = tracepoint_probe_register(tp_sched_exit,
					(void *)pacct_process_exit, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register for exit failed: %d\n", ret);
		goto err_tp_sched_fork;
	}

	ret = rapl_mod_init();
	if (ret) {
		pr_err("Failed to initialize RAPL events: %d\n", ret);
		goto err_tp_sched_exit;
	}

	// Start the energy estimator work
	pacct_start_energy_estimator();

	// Schedule a delayed work to scan existing tasks and create traced_task entries for them
	queue_paact_scan_tasks();

	return 0;

err_tp_sched_exit:
	if (tp_sched_exit)
		tracepoint_probe_unregister(tp_sched_exit,
					    (void *)pacct_process_exit, NULL);
err_tp_sched_fork:
	if (tp_sched_fork)
		tracepoint_probe_unregister(tp_sched_fork,
					    (void *)pacct_process_fork, NULL);
err_tp_sched_switch:
	if (tp_sched_switch)
		tracepoint_probe_unregister(tp_sched_switch,
					    (void *)pacct_sched_switch, NULL);
	// Clean up any traced tasks that might have been created before the failure
	clean_traced_task();
err:
	return ret;
}

static void __exit pacct_energy_exit(void)
{
	// Stop the energy estimator work by first
	pacct_stop_energy_estimator();

	// Wait for any pending work to finish
	rapl_mod_exit();

	if (tp_sched_switch)
		tracepoint_probe_unregister(tp_sched_switch,
					    (void *)pacct_sched_switch, NULL);

	if (tp_sched_fork)
		tracepoint_probe_unregister(tp_sched_fork,
					    (void *)pacct_process_fork, NULL);

	if (tp_sched_exit)
		tracepoint_probe_unregister(tp_sched_exit,
					    (void *)pacct_process_exit, NULL);

	// Clean up any traced tasks that might still be around
	powercap_cleanup_caps();

	// Clean up all traced tasks
	clean_traced_task();

	pr_info("pacct_energy removed\n");
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
