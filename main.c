#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/tracepoint.h>
#include <linux/smp.h>

#include "pacct.h"
#include "proc.h"
#include "cpu-class.h"
#include "errors.h"
#include "tracepoints.h"
#include "counters.h"

MODULE_AUTHOR("pm3");
MODULE_DESCRIPTION("Process Energy Accounting Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

// List of tasks being traced
struct list_head traced_tasks;
// List of tasks that are being retired (for cleanup)
struct list_head retiring_traced_tasks;
// Lock to protect access to the traced_tasks list
spinlock_t traced_tasks_lock;

// RAPL things
u64 last_pkg_raw, last_ns;

// Global statistics
struct stats global_stats = {};

// save stats for single run and accumulate
static int pacct_sched_switch_prev(struct task_struct *prev)
{
	struct traced_task *t_prev =
		get_or_create_traced_task(prev->pid, NULL, false);
	if (PACCT_ERROR_TRACE(sched_switch_prev_no_traced_task,
			      t_prev == NULL)) {
		return -1;
	}
	if (PACCT_ERROR_TRACE(sched_switch_prev_not_ready,
			      !READ_ONCE(t_prev->ready))) {
		goto err;
	}
	if (PACCT_ERROR_TRACE(sched_switch_prev_not_running,
			      !t_prev->running)) {
		goto err;
	}

	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (PACCT_ERROR_TRACE(sched_switch_prev_null_event,
				      t_prev->event[i] == NULL)) {
			goto err;
		}
	}
	u64 time_diff = ktime_get_ns() - t_prev->run_start_ns;
	u64 counter_diff[PACCT_TRACED_EVENT_COUNT];
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		// TODO verify that this method does the correc thing
		s64 diff = (s64)read_event_count(t_prev->event[i]) -
			   (s64)t_prev->counter_start[i];
		if (PACCT_ERROR_TRACE(sched_switch_prev_counter_got_smaller,
				      diff < 0)) {
			diff = 0; // TODO ?
		}
		counter_diff[i] = (u64)diff;

		// TODO perf_event_disable_local?
		// perf_event_disable(t_next->event[i]); TODO why not here? module freezes
	}
	// TODO verify that task_cpu returns the correct value even though we're not scheduled anymore
	enum pacct_cpu_class class = pacct_cpu_class_get(task_cpu(prev));
	// accumulate
	unsigned long flags;
	spin_lock_irqsave(&t_prev->periodic_lock, flags);
	if (class == PACCT_CPU_CLASS_EFFICIENCY) {
		t_prev->periodic_data.time_efficiency_ns += time_diff;
		for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
			t_prev->periodic_data.counter_diff_efficiency[i] +=
				counter_diff[i];
		}
	} else {
		t_prev->periodic_data.time_performance_ns += time_diff;
		for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
			t_prev->periodic_data.counter_diff_performance[i] +=
				counter_diff[i];
		}
	}
	spin_unlock_irqrestore(&t_prev->periodic_lock, flags);

	WRITE_ONCE(t_prev->running, false);
	return 0;
err:
	kref_put(&t_prev->ref_count, release_traced_task);
	return -1;
}

// setup next running process with current timestamp and counter values
static int pacct_sched_switch_next(struct task_struct *next)
{
	struct traced_task *t_next =
		get_or_create_traced_task(next->pid, NULL, false);
	if (PACCT_ERROR_TRACE(sched_switch_next_no_traced_task,
			      t_next == NULL)) {
		return -1;
	}
	if (PACCT_ERROR_TRACE(sched_switch_next_not_ready,
			      !READ_ONCE(t_next->ready))) {
		goto err;
	}
	if (PACCT_ERROR_TRACE(sched_switch_next_already_running,
			      t_next->running)) {
		goto err;
	}

	t_next->run_start_ns = ktime_get_ns();
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (PACCT_ERROR_TRACE(sched_switch_next_null_event,
				      t_next->event[i] == NULL)) {
			goto err;
		}
	}
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		// perf_event_enable(t_next->event[i]); TODO why not here? module freezes
		t_next->counter_start[i] = read_event_count(t_next->event[i]);
	}

	WRITE_ONCE(t_next->running, true);
	return 0;
err:
	kref_put(&t_next->ref_count, release_traced_task);
	return -1;
}

static void old_pacct_sched_switch(void *ignore, bool preempt,
				   struct task_struct *prev,
				   struct task_struct *next)
{
	// Don't trace kernel threads
	if (!(prev->flags & PF_KTHREAD))
		pacct_sched_switch_prev(prev);
	if (!(next->flags & PF_KTHREAD))
		pacct_sched_switch_next(next);
}

static void old_pacct_process_fork(void *ignore, struct task_struct *parent,
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

//move task from traced_tasks to retiring_traced_tasks
static void old_pacct_process_exit(void *ignore, struct task_struct *p)
{
	// Don't trace kernel threads
	if (p->flags & PF_KTHREAD)
		return;
	struct traced_task *e = get_or_create_traced_task(p->pid, NULL, false);
	if (!e)
		return;

	// Mark this task as retiring so that the sample_workfn can skip it if it hasn't run yet
	// TODO: Is it not already skipped by not being in the list?
	// TODO: How can the final counters be measured? Do even want to? (The proc file which would display the values will be deleted anyway)
	WRITE_ONCE(e->retiring, true);

	// remove from traced_tasks
	spin_lock(&traced_tasks_lock);
	list_del_init(&e->list);

	// add to retiring_traced_tasks for cleanup
	list_add_tail(&e->retire_node, &retiring_traced_tasks);

	spin_unlock(&traced_tasks_lock);

	// we'd got a ref from get_traced_task()
	kref_put(&e->ref_count, release_traced_task);
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

	ret = pacct_cpu_class_init();
	if (ret) {
		pr_err("could not initialize CPU classes\n");
		goto err;
	}
	ret = pacct_counters_install();
	if (ret) {
		pr_err("could not install counters\n");
		goto err;
	}
	pr_info("installed counters\n");
	ret = pacct_tracepoints_register();
	if (ret) {
		pr_err("could not register tracepoints\n");
		goto err_counters;
	}
	pr_info("registered tracepoints\n");

	// TODO this needs to happen before the tracpoints are registered
	// Initialize the list of traced tasks and the lock
	// spin_lock_init(&traced_tasks_lock);
	// INIT_LIST_HEAD(&traced_tasks);
	// INIT_LIST_HEAD(&retiring_traced_tasks);

	// Initialize the powercap interfaces and get the initial CPU frequency caps
	// TODO ret = powercap_init_caps();
	// if (ret) {
	// 	pr_err("powercap init failed: %d\n", ret);
	// 	goto err_counters;
	// }

	// init_proc(); // Create directory in proc/

	// Start the energy estimator work
	// pacct_start_energy_estimator();

	// Schedule a delayed work to scan existing tasks and create traced_task entries for them
	// queue_pacct_scan_tasks();

	return 0;

	// Clean up any traced tasks that might have been created before the failure
	// clean_traced_task();

err_tracepoints:
	pacct_tracepoints_unregister();
err_counters:
	pacct_counters_uninstall();
err:
	return ret;
}

static void __exit pacct_energy_exit(void)
{
	// Stop the energy estimator work by first
	// pacct_stop_energy_estimator();

	pacct_tracepoints_unregister();
	pacct_counters_uninstall();

	// Clean up for powercap policies and interfaces
	// powercap_cleanup_caps();

	// Clean up all traced tasks
	// clean_traced_task();

	// Clean up proc entries for all traced tasks
	// remove_proc();

	// pacct_error_report();

	pr_info("pacct_energy removed\n");
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
