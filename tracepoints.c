#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/tracepoint.h>

#include "events.h"
#include "counters.h"
#include "cpu-class.h"
#include "tracepoints.h"

atomic_t counter;
static void pacct_sched_switch(void *ignore, bool preempt,
			       struct task_struct *prev,
			       struct task_struct *next)
{
	// TODO real version
	int ret;
	int me = get_cpu();
	int my_class = pacct_cpu_class_get(me);
	if (atomic_fetch_add_relaxed(1, &counter) < 100) {
		pr_info("switching on CPU %d. I am a %s core.\n", me,
			(my_class == PACCT_CPU_CLASS_EFFICIENCY) ? "effi" :
								   "perf");
		u64 counters[NUM_EVENTS_MAX] = { 0 };
		u64 enabled[NUM_EVENTS_MAX] = { 0 };
		u64 running[NUM_EVENTS_MAX] = { 0 };
		for (int i = 0; i < NUM_EVENTS_MAX; i++) {
			if (pacct_perf_events[me][i] != NULL) {
				ret = perf_event_read_local(
					pacct_perf_events[me][i], &counters[i],
					&enabled[i], &running[i]);
				if (ret) {
					pr_err("core %d had trouble reading installed counter %d\n",
					       me, i);
				}
			}
		}
		pr_info("counters on my core %d: %llu(%llu/%llu) %llu(%llu/%llu) %llu(%llu/%llu) %llu(%llu/%llu)\n",
			me, counters[0], running[0], enabled[0], counters[1],
			running[1], enabled[1], counters[2], running[2],
			enabled[2], counters[3], running[3], enabled[3]);
	}
	put_cpu();
}

static void pacct_process_fork(void *ignore, struct task_struct *parent,
			       struct task_struct *child)
{
	// TODO real version
}
static void pacct_process_exit(void *ignore, struct task_struct *p)
{
	// TODO real version
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
