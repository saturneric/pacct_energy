#pragma once

#include <linux/list.h>
#include <linux/kref.h>

// Define the events we want to track with their event codes and umasks
static struct {
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

#define PACCT_TRACED_EVENT_COUNT ARRAY_SIZE(tracked_events)

struct traced_task {
	struct list_head list;
	struct hlist_node hnode;
	struct list_head retire_node; // Node for the retiring_traced_tasks list
	struct kref ref_count; // Reference count for this traced task entry
	pid_t pid;
	bool ready;
	bool needs_setup;
	struct perf_event *event[PACCT_TRACED_EVENT_COUNT];
	u64 counts[PACCT_TRACED_EVENT_COUNT];
};

struct traced_task *new_traced_task(pid_t pid);
void release_traced_task(struct kref *kref);
int setup_traced_task_counters(struct traced_task *entry);

struct task_struct *get_task_by_pid(pid_t pid);

void queue_pacct_setup_work(void);
void queue_pacct_retire_work(void);
