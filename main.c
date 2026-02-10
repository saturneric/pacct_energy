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

static struct tracepoint *tp_sched_switch; // Tracepoint for sched_switch events

struct list_head traced_tasks; // List of tasks being traced
spinlock_t traced_tasks_lock; // Lock to protect access to the traced_tasks list

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

static void tp_lookup_cb(struct tracepoint *tp, void *priv)
{
	const char *name = tp->name;
	if (name && strcmp(name, "sched_switch") == 0) {
		*(struct tracepoint **)priv = tp;
	}
}

static struct traced_task *get_or_create_traced_task(pid_t pid)
{
	struct traced_task *entry;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry(entry, &traced_tasks, list) {
		if (entry->pid == pid) {
			// Found an existing entry for this PID, increment refcount and return it
			goto out;
		}
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
	kref_get(&entry->ref);
err:
	spin_unlock(&traced_tasks_lock);
	return entry;
}

static void probe_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	struct traced_task *e = get_or_create_traced_task(prev->pid);
	if (!e)
		return;

	if (!READ_ONCE(e->ready)) {
		WRITE_ONCE(e->needs_setup, true);
		queue_pacct_setup_work();
		goto out;
	}

	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (e->event[i] && !IS_ERR(e->event[i]))
			e->counts[i] = read_event_count(e->event[i]);
	}

out:
	kref_put(&e->ref, release_traced_task);
}

static int __init pacct_energy_init(void)
{
	int ret;

	pr_info("pacct_energy init\n");

	// Initialize the list of traced tasks and the lock
	spin_lock_init(&traced_tasks_lock);
	INIT_LIST_HEAD(&traced_tasks);

	// Initialize the workqueue for setting up perf events
	ret = pacct_init_workqueue();
	if (ret) {
		pr_err("Failed to initialize workqueue: %d\n", ret);
		goto err;
	}

	for_each_kernel_tracepoint(tp_lookup_cb, &tp_sched_switch);
	if (!tp_sched_switch) {
		pr_err("tracepoint sched_switch not found (CONFIG_TRACEPOINTS/TRACE_EVENTS?)\n");
		ret = -ENOENT;
		goto err_wq;
	}

	// Register the probe function for the sched_switch tracepoint
	ret = tracepoint_probe_register(tp_sched_switch,
					(void *)probe_sched_switch, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register failed: %d\n", ret);
		goto err_wq;
	}

	return 0;

err_wq:
	pacct_cleanup_workqueue();
err:
	return ret;
}

static void __exit pacct_energy_exit(void)
{
	pr_info("pacct_energy removed\n");

	if (tp_sched_switch)
		tracepoint_probe_unregister(tp_sched_switch,
					    (void *)probe_sched_switch, NULL);

	// Disable and release all events
	struct list_head retiring_traced_tasks;
	INIT_LIST_HEAD(&retiring_traced_tasks);

	struct traced_task *entry, *tmp;
	spin_lock(&traced_tasks_lock);
	list_for_each_entry_safe(entry, tmp, &traced_tasks, list) {
		list_del(&entry->list);
		list_add(&entry->list, &retiring_traced_tasks);
	}
	spin_unlock(&traced_tasks_lock);

	list_for_each_entry_safe(entry, tmp, &retiring_traced_tasks, list) {
		list_del(&entry->list);
		kref_put(&entry->ref, release_traced_task);
	}

	pacct_cleanup_workqueue();
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
