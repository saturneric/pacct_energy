#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/tracepoint.h>

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

static u64 read_event_count(struct perf_event *ev)
{
	// the time (in perf time units) the event was enabled (counting or not)
	u64 enabled = 0;
	// the time (in perf time units) the event was actually running (counting)
	u64 running = 0;

	if (!ev)
		return 0;

	// Read the raw count and scale it based on the time the event was enabled and running
	u64 val = perf_event_read_value(ev, &enabled, &running);

	// Scale the raw count to account for time when the event was enabled but not running
	u64 scaled = (running ? div64_u64(val * enabled, running) : val);
	return scaled;
}

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

static void probe_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	// struct traced_task *e = get_or_create_traced_task(prev->pid);
	// if (!e) {
	// 	pr_err("Failed to get or create traced task for PID %d\n",
	// 	       prev->pid);
	// 	return;
	// }

	// if (!READ_ONCE(e->ready)) {
	// 	WRITE_ONCE(e->needs_setup, true);
	// 	goto out;
	// }

	// for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
	// 	if (e->event[i] && !IS_ERR(e->event[i]))
	// 		e->counts[i] = read_event_count(e->event[i]);
	// }

	// out:
	// 	kref_put(&e->ref_count, release_traced_task);
}

static void pacct_process_fork(void *ignore, struct task_struct *parent,
			       struct task_struct *child)
{
	// Don't trace kernel threads
	if (child->flags & PF_KTHREAD) {
		pr_info_ratelimited(
			"Skipping fork of kernel thread with PID %d\n",
			child->pid);
		goto out;
	}

	/* nicht schlafen */
	pr_info_ratelimited("Forked process: parent PID %d, child PID %d\n",
			    parent->pid, child->pid);

	struct traced_task *e = get_or_create_traced_task(child->pid, true);
	if (!e) {
		pr_err("Failed to get or create traced task for PID %d\n",
		       child->pid);
		return;
	}

	queue_pacct_setup_work();

out:
	kref_put(&e->ref_count, release_traced_task);
}

static void pacct_process_exit(void *ignore, struct task_struct *p)
{
	struct traced_task *e = get_traced_task(p->pid);
	if (!e) {
		pr_warn("Cannot find traced task for PID %d\n", p->pid);
		return;
	}

	struct traced_task *it;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry(it, &traced_tasks, list) {
		if (it->pid == p->pid) {
			e = it;
			// remove from traced_tasks
			list_del_init(&e->list);

			// add to retiring_traced_tasks for cleanup
			list_add_tail(&e->retire_node, &retiring_traced_tasks);

			// we'd got a ref from get_traced_task()
			kref_put(&e->ref_count, release_traced_task);
			break;
		}
	}
	spin_unlock(&traced_tasks_lock);
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
		pr_err("tracepoint sched_switch not found (CONFIG_TRACEPOINTS/TRACE_EVENTS?)\n");
		ret = -ENOENT;
		goto err;
	}

	for_each_kernel_tracepoint(tp_lookup_cb, "sched_process_fork");
	if (!tp_sched_fork) {
		pr_err("tracepoint sched_process_fork not found (CONFIG_TRACEPOINTS/TRACE_EVENTS?)\n");
		ret = -ENOENT;
		goto err;
	}

	for_each_kernel_tracepoint(tp_lookup_cb, "sched_process_exit");
	if (!tp_sched_exit) {
		pr_err("tracepoint sched_process_exit not found (CONFIG_TRACEPOINTS/TRACE_EVENTS?)\n");
		ret = -ENOENT;
		goto err;
	}

	// Register the probe function for the sched_switch tracepoint
	ret = tracepoint_probe_register(tp_sched_switch,
					(void *)probe_sched_switch, NULL);
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

	return 0;

err_tp_sched_fork:
	tracepoint_probe_unregister(tp_sched_fork, (void *)pacct_process_fork,
				    NULL);
err_tp_sched_switch:
	tracepoint_probe_unregister(tp_sched_switch, (void *)probe_sched_switch,
				    NULL);
err:
	return ret;
}

static void __exit pacct_energy_exit(void)
{
	if (tp_sched_switch)
		tracepoint_probe_unregister(tp_sched_switch,
					    (void *)probe_sched_switch, NULL);

	if (tp_sched_fork)
		tracepoint_probe_unregister(tp_sched_fork,
					    (void *)pacct_process_fork, NULL);

	if (tp_sched_exit)
		tracepoint_probe_unregister(tp_sched_exit,
					    (void *)pacct_process_exit, NULL);

	// Disable and release all events
	struct list_head retiring_traced_tasks;
	INIT_LIST_HEAD(&retiring_traced_tasks);

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
