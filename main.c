#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/printk.h>
#include <linux/tracepoint.h>

MODULE_AUTHOR("pm3");
MODULE_DESCRIPTION("Process Energy Accounting Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

static struct tracepoint *tp_sched_switch; // Tracepoint for sched_switch events

// Define the events we want to track with their event codes and umasks
static struct tracked_event {
	u8 event_code;
	u8 umask;
} tracked_events[8] = {
	// CPU_CLK_UNHALTED.THREAD_P
	{
		.event_code = 0x3c,
		.umask = 0x00,
	},
	// DTLB_STORE_MISSES.WALK_COMPLETED_4K
	{
		.event_code = 0x13,
		.umask = 0x02,
	},
	// BR_MISP_RETIRED.ALL_BRANCHES
	{
		.event_code = 0xc5,
		.umask = 0x00,
	},
	// INST_RETIRED.ANY_P
	{
		.event_code = 0xc0,
		.umask = 0x00,
	},
	// CPU_CLK_UNHALTED.C0_WAIT
	{
		.event_code = 0xec,
		.umask = 0x70,
	},
	// INT_MISC.UOP_DROPPING
	{
		.event_code = 0xad,
		.umask = 0x10,
	},
	// EXE_ACTIVITY.1_PORTS_UTIL
	{
		.event_code = 0x02,
		.umask = 0xa6,
	},
	// MEM_LOAD_RETIRED.L1_HIT
	{
		.event_code = 0xd1,
		.umask = 0x01,
	},
};

#define TRACED_EVENT_COUNT ARRAY_SIZE(tracked_events)

struct traced_task {
	struct list_head list;
	struct kref ref; // Reference count for this traced task entry
	pid_t pid;
	struct perf_event *event[TRACED_EVENT_COUNT];
	u64 counts[TRACED_EVENT_COUNT];
};

static void release_traced_task(struct kref *kref)
{
	struct traced_task *entry = container_of(kref, struct traced_task, ref);

	// Disable and release all events for this traced task
	for (int i = 0; i < TRACED_EVENT_COUNT; i++) {
		if (entry->event[i] && !IS_ERR(entry->event[i])) {
			perf_event_disable(entry->event[i]);
			perf_event_release_kernel(entry->event[i]);
		}
	}
	kfree(entry);
}

static struct list_head traced_tasks; // List of tasks being traced
static spinlock_t
	traced_tasks_lock; // Lock to protect access to the traced_tasks list

struct task_struct *get_task_by_pid(pid_t pid)
{
	struct task_struct *task = NULL;

	rcu_read_lock();
	task = pid_task(find_vpid(pid), PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task; // muss später put_task_struct()
}

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

static int setup_task_counter(pid_t pid, struct perf_event **event,
			      u8 event_code, u8 umask)
{
	int ret;
	struct perf_event_attr attr;
	struct task_struct *t;
	u64 raw = (u64)event_code | ((u64)umask << 8);

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_RAW;
	attr.config = raw;
	attr.size = sizeof(attr);

	attr.disabled = 1;
	attr.exclude_kernel = 0;
	attr.exclude_user = 0;
	attr.exclude_hv = 0;

	t = get_task_by_pid(pid);

	if (!t) {
		ret = -ESRCH;
		goto err;
	}

	*event = perf_event_create_kernel_counter(&attr, -1, t, NULL, NULL);
	put_task_struct(t);

	if (IS_ERR(*event)) {
		pr_err("Failed to create perf event for PID %d: %ld\n", pid,
		       PTR_ERR(*event));
		ret = -1;
		goto err;
	}

	perf_event_enable(*event);
	return 0;

err:
	return ret;
}

static int setup_traced_task_counters(struct traced_task *entry)
{
	for (int i = 0; i < TRACED_EVENT_COUNT; i++) {
		if (entry->event[i] && !IS_ERR(entry->event[i]))
			continue; // Counter already set up for this event

		if (setup_task_counter(entry->pid, &entry->event[i],
				       tracked_events[i].event_code,
				       tracked_events[i].umask) < 0) {
			pr_err("Failed to set up counter for PID %d event code 0x%02x umask 0x%02x\n",
			       entry->pid, tracked_events[i].event_code,
			       tracked_events[i].umask);
			return -1;
		}
	}
	return 0;
}

static struct traced_task *get_traced_task(pid_t pid)
{
	struct traced_task *entry;

	spin_lock(&traced_tasks_lock);

	list_for_each_entry(entry, &traced_tasks, list) {
		if (entry->pid == pid) {
			// Found an existing entry for this PID, increment refcount and return it
			kref_get(&entry->ref);
			spin_unlock(&traced_tasks_lock);
			return entry;
		}
	}

	// If the previous task is not being traced, set up counters for it
	// we should not sleep while holding the lock
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		pr_err("Failed to allocate memory for traced_task\n");
		spin_unlock(&traced_tasks_lock);
		return NULL;
	}

	kref_init(&entry->ref);
	entry->pid = pid;
	setup_traced_task_counters(entry);
	list_add(&entry->list, &traced_tasks);
	for (int i = 0; i < TRACED_EVENT_COUNT; i++) {
		entry->counts[i] = 0;
	}

	// Initialize refcount to 1 for the new entry
	kref_get(&entry->ref);
	spin_unlock(&traced_tasks_lock);
	return entry;
}

static void probe_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	struct traced_task *prev_entry = get_traced_task(prev->pid);
	if (!prev_entry) {
		pr_err("Failed to get traced task for PID %d\n", prev->pid);
		return;
	}

	for (int i = 0; i < TRACED_EVENT_COUNT; i++) {
		if (prev_entry->event[i] && !IS_ERR(prev_entry->event[i]))
			prev_entry->counts[i] =
				read_event_count(prev_entry->event[i]);
	}

	kref_put(&prev_entry->ref, release_traced_task);
}

static int __init pacct_energy_init(void)
{
	int ret;

	pr_info("pacct_energy init\n");

	// Initialize the list of traced tasks and the lock
	spin_lock_init(&traced_tasks_lock);
	INIT_LIST_HEAD(&traced_tasks);

	for_each_kernel_tracepoint(tp_lookup_cb, &tp_sched_switch);
	if (!tp_sched_switch) {
		pr_err("tracepoint sched_switch not found (CONFIG_TRACEPOINTS/TRACE_EVENTS?)\n");
		return -ENOENT;
	}

	// Register the probe function for the sched_switch tracepoint
	ret = tracepoint_probe_register(tp_sched_switch,
					(void *)probe_sched_switch, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register failed: %d\n", ret);
		return ret;
	}

	return 0; /* success */
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

	list_for_each_entry_safe(entry, tmp, &traced_tasks, list) {
		list_del(&entry->list);
		kref_put(&entry->ref, release_traced_task);
	}
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
