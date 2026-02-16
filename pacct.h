#pragma once

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/workqueue.h>

// Define the events we want to track with their event codes and umasks
static struct {
	u8 event_code;
	u8 umask;
	s64 koeff; // Coefficient for energy estimation
} tracked_events[] = {
	// CPU_CLK_UNHALTED.THREAD_P
	{
		.event_code = 0x3c,
		.umask = 0x00,
		.koeff = 63840,
	},
	// INST_RETIRED.ANY_P
	{
		.event_code = 0xc0,
		.umask = 0x00,
		.koeff = 491702,
	},
	// OFFCORE_REQUESTS_OUTSTANDING.DEMAND_DATA_RD
	{
		.event_code = 0x20,
		.umask = 0x01,
		.koeff = -239221,
	},
	// BR_INST_RETIRED.ALL_BRANCHES
	{
		.event_code = 0xc4,
		.umask = 0x00,
		.koeff = 226492,
	},
	// MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM
	{
		.event_code = 0xd3,
		.umask = 0x01,
		.koeff = 202299,
	},
	// INST_RETIRED.ANY
	// Number of instructions retired. Fixed Counter - architectural event
	{
		.event_code = 0x00,
		.umask = 0x01,
		.koeff = -178523, // Why negative?
	},
	// EOFFCORE_REQUESTS.DEMAND_DATA_RD
	{
		.event_code = 0x21,
		.umask = 0x01,
		.koeff = -151731,
	},
	// EXE_ACTIVITY.1_PORTS_UTIL
	{
		.event_code = 0xa6,
		.umask = 0x02,
		.koeff = 138130,
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
	bool retiring; // Flag to indicate if this task is being retired and should not be sampled anymore
	bool needs_setup;
	struct perf_event *event[PACCT_TRACED_EVENT_COUNT];

	// pref counts for each event, updated on context switches
	u64 counts[PACCT_TRACED_EVENT_COUNT];
	// estimated energy consumption based on the diff counts and coefficients
	atomic64_t diff_counts[PACCT_TRACED_EVENT_COUNT];

	// Execution runtime tracking for power estimation
	u64 last_exec_runtime;
	atomic64_t delta_exec_runtime_acc;
	u64 total_exec_runtime_acc;

	// Wall clock timestamp of the last context switch for this task, also used for power estimation
	atomic64_t last_timestamp_ns;
	atomic64_t delta_timestamp_acc;

	// estimated energy consumption
	atomic64_t energy;
	// estimated avg power consumption (based on execution runtime)
	atomic64_t power_a;
	// estimated instant power consumption (based on execution runtime)
	atomic64_t power_i;
	// estimated power consumption based on wall clock time - this can help us
	// capture power of sleeping tasks which might not have much execution runtime
	// but can still consume power due to background activity like memory accesses
	atomic64_t power_w;

	// Number of times this task has been recorded in the energy estimation work.
	atomic_t record_count;

	char comm[TASK_COMM_LEN];
};

struct traced_task *new_traced_task(pid_t pid);
void release_traced_task(struct kref *kref);
int setup_traced_task_counters(struct traced_task *entry);
struct traced_task *get_or_create_traced_task(pid_t pid, const char *comm,
					      bool create);

void queue_pacct_setup_work(void);
void queue_pacct_retire_work(void);
void queue_paact_scan_tasks(void);
void pacct_start_energy_estimator(void);
void pacct_stop_energy_estimator(void);

struct task_struct *get_task_by_pid(pid_t pid);
u64 read_event_count(struct perf_event *ev);