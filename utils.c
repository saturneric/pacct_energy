#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include "pacct.h"

#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/task.h>
#include <linux/perf_event.h>
#include <linux/math64.h>

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

u64 read_event_count(struct perf_event *ev)
{
	// the time (in perf time units) the event was enabled (counting or not)
	u64 enabled = 0;
	// the time (in perf time units) the event was actually running (counting)
	u64 running = 0;

	if (!ev)
		return 0;

	// Read the raw count and scale it based on the time the event was enabled and running
	u64 val;
	int ret = perf_event_read_local(ev, &val, &enabled, &running);
	if (ret)
		return 0;

	// Scale the raw count to account for time when the event was enabled but not running
	u64 scaled =
		(running ? mul_u64_u64_div_u64(val, enabled, running) : val);
	return scaled;
}

u64 read_event_count_sleepable(struct perf_event *ev)
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
	u64 scaled =
		(running ? mul_u64_u64_div_u64(val, enabled, running) : val);
	return scaled;
}

void print_stats(struct stats *s)
{
	pr_info("=== Global Statistics ===\n");
	pr_info("Energy (estimated): %lld uJ\n", s->energy);
	pr_info("Energy (RAPL)     : %lld uJ\n", s->energy_rapl);
	pr_info("Power (estimated): %lld mW\n", s->power);
	pr_info("Power (RAPL)     : %lld mW\n", s->power_rapl);
	pr_info("Counters:\n");
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		pr_info("  r%02x%02x: %lld\n", 
			tracked_events[i].umask,
			tracked_events[i].event_code,
			s->counter[i]);
	}
}