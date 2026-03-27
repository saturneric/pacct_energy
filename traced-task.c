#include "traced-task.h"

// List of tasks being traced
struct list_head pacct_traced_tasks;
// List of tasks that are being retired (for cleanup)
struct list_head pacct_retiring_traced_tasks;
// Lock to protect access to the traced_tasks list
spinlock_t pacct_traced_tasks_lock;

static void pacct_traced_tasks_init(void)
{
	spin_lock_init(&pacct_traced_tasks_lock);
	INIT_LIST_HEAD(&pacct_traced_tasks);
	INIT_LIST_HEAD(&pacct_retiring_traced_tasks);
}

static void pacct_traced_tasks_clean(void)
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

static struct pacct_traced_task *pacct_traced_task_create(pid_t pid)
{
	struct pacct_traced_task *entry;

	// Allocate and initialize a new traced_task entry
	// Use GFP_ATOMIC since this will be called from an atomic context
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		pr_err("Failed to allocate memory for traced_task\n");
		return NULL;
	}
	// all other fields are 0-initialized here because of `kzalloc`.
	kref_init(&entry->ref_count);
	entry->pid = pid;
	spin_lock_init(&entry->periodic_lock);
	return entry;
}

int pacct_traced_task_setup(struct pacct_traced_task *entry)
{
	return pacct_proc_file_setup(entry);
}

struct pacct_traced_task *pacct_traced_task_get(pid_t pid)
{
	struct pacct_traced_task *entry;

	spin_lock(&pacct_traced_tasks_lock);
	list_for_each_entry(entry, &pacct_traced_tasks, list) {
		if (entry->pid == pid) {
			goto out;
		}
	}
	// haven't found anything.
	entry = NULL;
	goto err;

out:
	kref_get(&entry->ref_count);
err:
	spin_unlock(&pacct_traced_tasks_lock);
	return entry;
}

void pacct_traced_task_release(struct kref *kref)
{
	struct pacct_traced_task *entry =
		container_of(kref, struct pacct_traced_task, ref_count);

	pacct_proc_file_free(entry);

	kfree(entry);
}
