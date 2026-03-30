#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/tracepoint.h>

#include "events.h"
#include "counters.h"
#include "cpu-class.h"
#include "traced-task.h"
#include "errors.h"
#include "tracepoints.h"
#include "wq.h"

// for testing
struct task_struct *temp_first_scheduled_ts = NULL;
struct pacct_traced_task *temp_first_scheduled = NULL;

// save stats for single run and accumulate
static int pacct_sched_switch_prev(struct task_struct *prev)
{
	if (prev == temp_first_scheduled_ts) {
		pr_info("descheduling test-traced task\n");
	}
	struct pacct_traced_task *t_prev = NULL;
	int ret = pacct_traced_task_get_or_create(prev->pid, true, &t_prev);
	if (PACCT_ERROR_TRACE(sched_switch_prev_no_traced_task,
			      ret || t_prev == NULL)) {
		return -1;
	}

	// NOTE: removed check for ready because we don't need tasks to be ready here anymore
	if (PACCT_ERROR_TRACE(sched_switch_prev_not_running,
			      !t_prev->running)) {
		// this is not necessarily an error condition.
		// May happen if we encounter this task for the first time.
		goto err;
	}
	if (PACCT_ERROR_TRACE(sched_switch_prev_wrong_cpu,
			      task_cpu(prev) != t_prev->current_cpu)) {
		goto err;
	}

	enum pacct_cpu_class class = pacct_cpu_class_get(task_cpu(prev));
	u64 time_diff = ktime_get_ns() - t_prev->run_start_ns;
	u64 counter_diff[PACCT_NUM_EVENTS_MAX];
	for (int i = 0; i < PACCT_NUM_EVENTS_ON_CLASS(class); i++) {
		s64 diff = (s64)pacct_counter_read_local(i) -
			   (s64)t_prev->counter_start[i];
		if (PACCT_ERROR_TRACE(sched_switch_prev_counter_got_smaller,
				      diff < 0)) {
			diff = 0;
		}
		counter_diff[i] = (u64)diff;
	}
	// accumulate
	unsigned long flags;
	// TODO is this the right variant?
	spin_lock_irqsave(&t_prev->periodic_lock, flags);
	if (class == PACCT_CPU_CLASS_EFFICIENCY) {
		t_prev->periodic_data.time_efficiency_ns += time_diff;
		for (int i = 0; i < PACCT_NUM_EVENTS_EFFICIENCY; i++) {
			t_prev->periodic_data.counter_diff_efficiency[i] +=
				counter_diff[i];
		}
	} else {
		t_prev->periodic_data.time_performance_ns += time_diff;
		for (int i = 0; i < PACCT_NUM_EVENTS_PERFORMANCE; i++) {
			t_prev->periodic_data.counter_diff_performance[i] +=
				counter_diff[i];
		}
	}
	spin_unlock_irqrestore(&t_prev->periodic_lock, flags);

	WRITE_ONCE(t_prev->running, false);
	kref_put(&t_prev->ref_count, pacct_traced_task_release);
	return 0;
err:
	kref_put(&t_prev->ref_count, pacct_traced_task_release);
	return -1;
}

// setup next running process with current timestamp and counter values
static int pacct_sched_switch_next(struct task_struct *next)
{
	// TODO this is just so we have _some_ output
	if (temp_first_scheduled_ts == next) {
		pr_info("scheduling test-traced task");
	}
	struct pacct_traced_task *t_next = NULL;
	int ret = pacct_traced_task_get_or_create(next->pid, true, &t_next);
	if (PACCT_ERROR_TRACE(sched_switch_next_no_traced_task,
			      ret || t_next == NULL)) {
		return -1;
	}
	if (PACCT_ERROR_TRACE(sched_switch_next_already_running,
			      t_next->running)) {
		goto err;
	}

	// TODO this is just so we have _some_ output
	if (temp_first_scheduled == NULL) {
		temp_first_scheduled = t_next;
		temp_first_scheduled_ts = next;
		pr_info("scheduling test-traced task");
	}

	int me = get_cpu();
	t_next->run_start_ns = ktime_get_ns();
	for (int i = 0; i < PACCT_NUM_EVENTS_ON_CPU(me); i++) {
		t_next->counter_start[i] = pacct_counter_read_local(i);
	}

	t_next->current_cpu = me; // only for testing
	put_cpu();
	WRITE_ONCE(t_next->running, true);
	kref_put(&t_next->ref_count, pacct_traced_task_release);
	return 0;
err:
	kref_put(&t_next->ref_count, pacct_traced_task_release);
	return -1;
}

static void pacct_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	// Don't trace kernel threads
	if (!(prev->flags & PF_KTHREAD))
		pacct_sched_switch_prev(prev);
	if (!(next->flags & PF_KTHREAD))
		pacct_sched_switch_next(next);
}

static void pacct_process_fork(void *ignore, struct task_struct *parent,
			       struct task_struct *child)
{
	// Don't trace kernel threads
	if (child->flags & PF_KTHREAD)
		return;

	int ret = pacct_traced_task_get_or_create(child->pid, true, NULL);
	if (ret) {
		pr_err("could not create traced task for PID %d\n", child->pid);
		return;
	}
	// pacct_queue_setup_work(); TODO removed here for now
}

//move task from traced_tasks to retiring_traced_tasks
static void pacct_process_exit(void *ignore, struct task_struct *p)
{
	// Don't trace kernel threads
	if (p->flags & PF_KTHREAD)
		return;
	struct pacct_traced_task *e = NULL;
	int ret = pacct_traced_task_get_or_create(p->pid, false, &e);
	if (PACCT_ERROR_TRACE(sched_exit_no_traced_task, ret)) {
		return;
	}

	// Mark this task as retiring so that the sample_workfn can skip it if it hasn't run yet
	// TODO: Is it not already skipped by not being in the list?
	// TODO: How can the final counters be measured? Do even want to? (The proc file which would display the values will be deleted anyway)
	WRITE_ONCE(e->retiring, true);

	// remove from traced_tasks
	spin_lock(&pacct_traced_tasks_lock);
	list_del_init(&e->list);

	// add to retiring_traced_tasks for cleanup
	list_add_tail(&e->retire_node, &pacct_retiring_traced_tasks);

	spin_unlock(&pacct_traced_tasks_lock);

	kref_put(&e->ref_count, pacct_traced_task_release);
}

static struct tracepoint *tp_sched_switch = NULL;
static struct tracepoint *tp_sched_exit = NULL;
static struct tracepoint *tp_sched_fork = NULL;

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

int pacct_tracepoints_register(void)
{
	int ret;

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
	ret = tracepoint_probe_register(tp_sched_switch,
					(void *)pacct_sched_switch, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register for switch failed\n");
		goto err;
	}

	ret = tracepoint_probe_register(tp_sched_fork,
					(void *)pacct_process_fork, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register for fork failed\n");
		goto err;
	}

	ret = tracepoint_probe_register(tp_sched_exit,
					(void *)pacct_process_exit, NULL);
	if (ret) {
		pr_err("tracepoint_probe_register for exit failed\n");
		goto err;
	}
	return 0;
err:
	pacct_tracepoints_unregister();
	return ret;
}

#define UNREGISTER(tp, func)                                                   \
	do {                                                                   \
		if (tp != NULL) {                                              \
			tracepoint_probe_unregister(tp, (void *)(func), NULL); \
			(tp) = NULL;                                           \
		}                                                              \
	} while (0)

void pacct_tracepoints_unregister(void)
{
	UNREGISTER(tp_sched_switch, pacct_sched_switch);
	UNREGISTER(tp_sched_fork, pacct_process_fork);
	UNREGISTER(tp_sched_exit, pacct_process_exit);
}
