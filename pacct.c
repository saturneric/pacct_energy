#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include "pacct.h"

#include <linux/perf_event.h>

struct traced_task *new_traced_task(pid_t pid)
{
	struct traced_task *entry;

	// Allocate and initialize a new traced_task entry
	// Use GFP_ATOMIC since this will be called from an atomic context
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		pr_err("Failed to allocate memory for traced_task\n");
		return NULL;
	}

	kref_init(&entry->ref_count);
	entry->pid = pid;
	entry->ready = false;
	entry->needs_setup = true;
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		entry->event[i] = NULL;
		entry->counts[i] = 0;
	}

	return entry;
}

void release_traced_task(struct kref *kref)
{
	struct traced_task *entry =
		container_of(kref, struct traced_task, ref_count);

	// Disable and release all events for this traced task
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (entry->event[i] && !IS_ERR(entry->event[i])) {
			perf_event_disable(entry->event[i]);
			perf_event_release_kernel(entry->event[i]);
		}
	}

	kfree(entry);
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

int setup_traced_task_counters(struct traced_task *entry)
{
	int ret;
	kref_get(&entry->ref_count);
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (entry->event[i] && !IS_ERR(entry->event[i]))
			continue; // Counter already set up for this event

		ret = setup_task_counter(entry->pid, &entry->event[i],
					 tracked_events[i].event_code,
					 tracked_events[i].umask);
		if (ret < 0) {
			pr_err("Failed to set up counter for PID %d event code 0x%02x "
			       "umask 0x%02x ret %d\n",
			       entry->pid, tracked_events[i].event_code,
			       tracked_events[i].umask, ret);
			goto err;
		}
	}
	return 0;
err:
	kref_put(&entry->ref_count, release_traced_task);
	return ret;
}