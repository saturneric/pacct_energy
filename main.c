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

// RAPL things
u64 last_pkg_raw, last_ns;

// Global statistics
struct stats global_stats = {};

// save stats for single run and accumulate
static int pacct_sched_switch_prev(struct task_struct *prev)
{
	struct traced_task *t_prev =
		get_or_create_traced_task(prev->pid, NULL, false);
	if (t_prev == NULL) {
		pacct_errors.sched_switch_prev_no_traced_task++;
		return -1;
	}
	if (!READ_ONCE(t_prev->ready)) {
		pacct_errors.sched_switch_prev_not_ready++;
		goto err;
	}
	if (!t_prev->running) {
		pacct_errors.sched_switch_prev_not_running++;
		goto err;
	}

	u64 time_diff = ktime_get_ns() - t_prev->run_start_ns;
	u64 counter_diff[PACCT_TRACED_EVENT_COUNT];
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		// TODO verify that this method does the correc thing
		s64 diff = (s64)read_event_count(t_prev->event[i]) -
			   (s64)t_prev->counter_start[i];
		if (diff < 0) {
			pacct_errors.sched_switch_prev_counter_got_smaller++;
			diff = 0; // TODO ?
		}
		counter_diff[i] = (u64)diff;
	}
	// TODO verify that task_cpu returns the correct value even though we're not scheduled anymore
	enum pacct_cpu_class class = pacct_cpu_class_get(task_cpu(prev));
	// accumulate
	unsigned long flags;
	spin_lock_irqsave(&t_prev->periodic_lock, flags);
	if (class == PACCT_CPU_CLASS_NONE) {
		pacct_errors.sched_switch_prev_invalid_cpu_class++;
	} else if (class == PACCT_CPU_CLASS_EFFICIENCY) {
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
	if (t_next == NULL) {
		pacct_errors.sched_switch_next_no_traced_task++;
		return -1;
	}
	if (!READ_ONCE(t_next->ready)) {
		pacct_errors.sched_switch_next_not_ready++;
		goto err;
	}
	if (t_next->running) {
		pacct_errors.sched_switch_next_already_running++;
		goto err;
	}

	t_next->run_start_ns = ktime_get_ns();
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		t_next->counter_start[i] = read_event_count(t_next->event[i]);
	}

	WRITE_ONCE(t_next->running, true);
	return 0;
err:
	kref_put(&t_next->ref_count, release_traced_task);
	return -1;
}

static void pacct_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	// Don't trace kernel threads
	if (!(prev->flags & PF_KTHREAD))
		pacct_sched_switch_prev(prev);
	if (!(next->flags & PF_KTHREAD))
		pacct_sched_switch_next(next);
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

//move task from traced_tasks to retiring_traced_tasks
static void pacct_process_exit(void *ignore, struct task_struct *p)
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

//Looks for the wanted tracepoints and store in static variables
static void tp_lookup_cb(struct tracepoint *tp, void *priv)
{
	(void)priv;
	if (!strcmp(tp->name, "sched_switch"))
		tp_sched_switch = tp;
	else if (!strcmp(tp->name, "sched_process_fork"))
		tp_sched_fork = tp;
	else if (!strcmp(tp->name, "sched_process_exit"))
		tp_sched_exit = tp;
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

static int __init pacct_energy_init(void) //Start of the module
{
	int ret;

	pr_info("pacct_energy init\n");

	// Classify CPUs
	ret = pacct_cpu_class_init();
	if (ret) {
		pr_err("could not initialize CPU classes\n");
		goto err;
	}

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

	//find the needed tracepoints
	for_each_kernel_tracepoint(tp_lookup_cb, NULL);
	if (!tp_sched_switch) {
		pr_err("tracepoint sched_switch not found\n");
		ret = -ENOENT;
		goto err;
	}
	if (!tp_sched_fork) {
		pr_err("tracepoint sched_process_fork not found\n");
		ret = -ENOENT;
		goto err;
	}
	if (!tp_sched_exit) {
		pr_err("tracepoint sched_process_exit not found\n");
		ret = -ENOENT;
		goto err;
	}

	// Register the functions to be called on the trace points
	ret = tracepoint_probe_register(tp_sched_switch,
					(void *)pacct_sched_switch, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register for switch failed: %d\n",
		       ret);
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

	init_proc(); // Create directory in proc/

	// Start the energy estimator work
	pacct_start_energy_estimator();

	// Schedule a delayed work to scan existing tasks and create traced_task entries for them
	queue_pacct_scan_tasks();

	return 0;

err_tp_sched_fork:
	tracepoint_probe_unregister(tp_sched_fork, (void *)pacct_process_fork,
				    NULL);
err_tp_sched_switch:
	tracepoint_probe_unregister(tp_sched_switch, (void *)pacct_sched_switch,
				    NULL);
	// Clean up any traced tasks that might have been created before the failure
	clean_traced_task();
err:
	return ret;
}

static void __exit pacct_energy_exit(void)
{
	// Stop the energy estimator work by first
	pacct_stop_energy_estimator();

	if (tp_sched_switch)
		tracepoint_probe_unregister(tp_sched_switch,
					    (void *)pacct_sched_switch, NULL);
	if (tp_sched_fork)
		tracepoint_probe_unregister(tp_sched_fork,
					    (void *)pacct_process_fork, NULL);

	if (tp_sched_exit)
		tracepoint_probe_unregister(tp_sched_exit,
					    (void *)pacct_process_exit, NULL);

	// Clean up for powercap policies and interfaces
	powercap_cleanup_caps();

	// Clean up all traced tasks
	clean_traced_task();

	// Clean up proc entries for all traced tasks
	remove_proc();

	pacct_error_report();

	pr_info("pacct_energy removed\n");
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
