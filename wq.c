#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/hashtable.h>

#include "pacct.h"

extern struct list_head traced_tasks;
extern spinlock_t traced_tasks_lock;

static atomic_t need_work = ATOMIC_INIT(0);

static void pacct_setup_workfn(struct work_struct *work)
{
	struct traced_task *e;
	for (;;) {
		atomic_set(&need_work, 0);

		// unter Lock einen Kandidaten finden und ref nehmen
		spin_lock(&traced_tasks_lock);
		list_for_each_entry(e, &traced_tasks, list) {
			if (!e->ready && e->needs_setup) {
				kref_get(&e->ref_count);

				e->needs_setup = false;
				WRITE_ONCE(e->ready,
					   setup_traced_task_counters(e) == 0);

				pr_info_ratelimited(
					"Setup perf events for PID %d: %s\n",
					e->pid,
					e->ready ? "success" : "failure");

				kref_put(&e->ref_count, release_traced_task);
			}
		}
		spin_unlock(&traced_tasks_lock);

		if (!atomic_xchg(&need_work, 0))
			break;
		atomic_set(&need_work, 1);
	}
}

static DECLARE_WORK(pacct_setup_work, pacct_setup_workfn);

void queue_pacct_setup_work(void)
{
	atomic_set(&need_work, 1);
	queue_work(system_unbound_wq, &pacct_setup_work);
}