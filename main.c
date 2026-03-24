#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/tracepoint.h>
#include <linux/smp.h>

#include "pacct.h"
#include "proc.h"
#include "cpu-class.h"

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

static void pacct_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	struct traced_task *e =
		get_or_create_traced_task(prev->pid, NULL, false);
	if (!e)
		return;

	if (!READ_ONCE(e->ready)) {
		// TODO should we check that this doesn't happen too often?
		goto out;
	}

	unsigned int cpu = task_cpu(prev);
	// TODO	find out on which CPU type the task ran, get timestamp and save somewhere
	// if the core type changed (?or is subject to change?), read counter values

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

//move task from traced_tasks to retiring_traced_tasks
static void pacct_process_exit(void *ignore, struct task_struct *p)
{
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

	pr_info("pacct_energy removed\n");
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
