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

static struct workqueue_struct *pacct_wq;
static atomic_t setup_pending = ATOMIC_INIT(0);

static void pacct_setup_workfn(struct work_struct *work)
{
	struct traced_task *e;

	// Job darf später erneut geplant werden
	atomic_set(&setup_pending, 0);

	for (;;) {
		e = NULL;

		// unter Lock einen Kandidaten finden und ref nehmen
		spin_lock(&traced_tasks_lock);
		list_for_each_entry(e, &traced_tasks, list) {
			if (!e->ready && e->needs_setup) {
				e->needs_setup = false;
				kref_get(&e->ref); // Work hält Referenz
				break;
			}
			e = NULL;
		}
		spin_unlock(&traced_tasks_lock);

		if (!e)
			break;

		// ohne Lock: perf events anlegen (darf schlafen)
		if (setup_traced_task_counters(e) == 0)
			WRITE_ONCE(e->ready, true);
		else
			WRITE_ONCE(e->ready, false);

		pr_info("Setup perf events for PID %d: %s\n", e->pid,
			e->ready ? "success" : "failure");

		kref_put(&e->ref, release_traced_task);
	}
}

static DECLARE_WORK(pacct_setup_work, pacct_setup_workfn);

void queue_pacct_setup_work(void)
{
	if (atomic_xchg(&setup_pending, 1) == 0)
		queue_work(pacct_wq, &pacct_setup_work);
}

int pacct_init_workqueue(void)
{
	pacct_wq = alloc_workqueue("pacct_wq", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!pacct_wq)
		return -ENOMEM;
	return 0;
}

void pacct_cleanup_workqueue(void)
{
	if (pacct_wq) {
		// warten, bis alle Jobs fertig sind
		flush_workqueue(pacct_wq);
		destroy_workqueue(pacct_wq);
		pacct_wq = NULL;
	}
}