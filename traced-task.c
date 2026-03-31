#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/slab.h>

#include "proc.h"
#include "traced-task.h"

// List of tasks being traced
struct list_head pacct_traced_tasks;
// List of tasks that are being retired (for cleanup)
struct list_head pacct_retiring_traced_tasks;
// Lock to protect access to the traced_tasks list
spinlock_t pacct_traced_tasks_lock;

void pacct_traced_tasks_init(void)
{
	spin_lock_init(&pacct_traced_tasks_lock);
	INIT_LIST_HEAD(&pacct_traced_tasks);
	INIT_LIST_HEAD(&pacct_retiring_traced_tasks);
}

void pacct_traced_tasks_clean(void)
{
	struct pacct_traced_task *entry, *tmp;
	spin_lock(&pacct_traced_tasks_lock);
	list_for_each_entry_safe(entry, tmp, &pacct_traced_tasks, list) {
		list_del_init(&entry->list);
		list_add_tail(&entry->retire_node,
			      &pacct_retiring_traced_tasks);
	}
	spin_unlock(&pacct_traced_tasks_lock);
}

int pacct_traced_task_setup(struct pacct_traced_task *entry)
{
	return pacct_proc_file_setup(entry);
}

int pacct_traced_task_get_or_create(pid_t pid, bool create,
				    struct pacct_traced_task **out)
{
	if (out) {
		*out = NULL;
	}
	struct pacct_traced_task *entry;

	unsigned long flags;
	spin_lock_irqsave(&pacct_traced_tasks_lock, flags);
	list_for_each_entry(entry, &pacct_traced_tasks, list) {
		if (entry->pid == pid) {
			goto out;
		}
	}
	// haven't found anything.
	if (!create) {
		spin_unlock_irqrestore(&pacct_traced_tasks_lock, flags);
		return -ESRCH;
	}
	// Allocate and initialize a new traced_task entry
	// Use GFP_ATOMIC since this will be called from an atomic context
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		pr_err("Failed to allocate memory for traced_task\n");
		spin_unlock_irqrestore(&pacct_traced_tasks_lock, flags);
		return -ENOMEM;
	}
	// all other fields are 0-initialized here because of `kzalloc`.
	kref_init(&entry->ref_count);
	entry->pid = pid;
	spin_lock_init(&entry->periodic_lock);
	// start period now
	memset(&entry->periodic_data, 0, sizeof(entry->periodic_data));
	entry->periodic_data.time_start_ns = ktime_get_ns();

	list_add_tail(&entry->list, &pacct_traced_tasks);
out:
	kref_get(&entry->ref_count);
	spin_unlock_irqrestore(&pacct_traced_tasks_lock, flags);

	if (out != NULL) {
		*out = entry;
	} else {
		kref_put(&entry->ref_count, pacct_traced_task_release);
	}
	return 0;
}

// TODO the release function should be atomic! move to retiring list
void pacct_traced_task_release(struct kref *kref)
{
	struct pacct_traced_task *entry =
		container_of(kref, struct pacct_traced_task, ref_count);

	pacct_proc_file_free(entry);

	kfree(entry);
}
