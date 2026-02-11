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

static struct traced_task *get_or_create_traced_task(pid_t pid, bool create)
{
	struct traced_task *entry;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry(entry, &traced_tasks, list) {
		if (entry->pid == pid) {
			// Found an existing entry for this PID, increment refcount and return it
			goto out;
		}
	}

	if (!create) {
		// No existing entry found and creation not allowed, return NULL
		entry = NULL;
		goto err;
	}

	// No existing entry found, create a new one
	entry = new_traced_task(pid);
	if (!entry) {
		pr_err("Failed to create traced task for PID %d\n", pid);
		goto err;
	}

	list_add(&entry->list, &traced_tasks);

out:
	// Increment refcount for the new entry
	kref_get(&entry->ref_count);
err:
	spin_unlock(&traced_tasks_lock);
	return entry;
}

static struct traced_task *get_traced_task(pid_t pid)
{
	return get_or_create_traced_task(pid, false);
}

static __inline__ u64 u64_delta_sat(u64 now, u64 prev)
{
	return (now >= prev) ? (now - prev) : 0;
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

	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		struct perf_event *ev = READ_ONCE(e->event[i]);
		if (ev && !IS_ERR(ev)) {
			u64 val = read_event_count(ev);
			u64 diff = u64_delta_sat(val, READ_ONCE(e->counts[i]));
			WRITE_ONCE(e->diff_counts[i], diff);
			WRITE_ONCE(e->counts[i], val);
		}
	}

	WRITE_ONCE(e->energy_updated, false);

out:
	kref_put(&e->ref_count, release_traced_task);
}

static void pacct_process_fork(void *ignore, struct task_struct *parent,
			       struct task_struct *child)
{
	// Don't trace kernel threads
	if (child->flags & PF_KTHREAD)
		return;

	struct traced_task *e = get_or_create_traced_task(child->pid, true);
	if (!e) {
		pr_err("Failed to get or create traced task for PID %d\n",
		       child->pid);
		return;
	}

	pr_info("Start to trace new process: PID %d, COMM %s\n", child->pid,
		child->comm);

	// schedule setup work for the new task to initialize its perf events
	queue_pacct_setup_work();

	kref_put(&e->ref_count, release_traced_task);
}

static void pacct_process_exit(void *ignore, struct task_struct *p)
{
	struct traced_task *e = get_traced_task(p->pid);
	if (!e)
		return;

	// Mark this task as retiring so that the sample_workfn can skip it if it hasn't run yet
	WRITE_ONCE(e->retiring, true);

	// remove from traced_tasks
	spin_lock(&traced_tasks_lock);
	list_del_init(&e->list);
	spin_unlock(&traced_tasks_lock);

	// print debug info about the exiting task
	pr_info("Process exiting: PID %d, COMM \"%s\", energy estimate %llu (uJ)\n",
		e->pid, p->comm, atomic64_read(&e->energy));

	// If energy is not zero, print the final event counts and diffs for this task
	// This can help us understand the event activity of the task.
	if (atomic64_read(&e->energy) != 0) {
		pr_info("Event counts for exiting PID %d:\n", e->pid);
		// print each event's final count and diff for this exiting task
		for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
			pr_info("[%d]: count=%llu, diff=%llu\n", i,
				e->counts[i], e->diff_counts[i]);
		}
	}

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

static int __init pacct_energy_init(void)
{
	int ret;

	pr_info("pacct_energy init\n");

	// Initialize the list of traced tasks and the lock
	spin_lock_init(&traced_tasks_lock);
	INIT_LIST_HEAD(&traced_tasks);
	INIT_LIST_HEAD(&retiring_traced_tasks);

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

	// Start the energy estimator work
	pacct_start_energy_estimator();

	return 0;

err_tp_sched_fork:
	tracepoint_probe_unregister(tp_sched_fork, (void *)pacct_process_fork,
				    NULL);
err_tp_sched_switch:
	tracepoint_probe_unregister(tp_sched_switch, (void *)pacct_sched_switch,
				    NULL);
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

	// Move all currently traced tasks to the retiring list for cleanup
	struct traced_task *entry, *tmp;
	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(entry, tmp, &traced_tasks, list) {
		list_del_init(&entry->list);
		list_add_tail(&entry->retire_node, &retiring_traced_tasks);
	}
	spin_unlock(&traced_tasks_lock);

	// Process retiring tasks to clean up their perf events
	queue_pacct_retire_work();

	pr_info("pacct_energy removed\n");
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
