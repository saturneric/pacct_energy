#pragma once

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>

#define COUNTER_SCALE 100000000
#define SCALE_COUNTER(counter) ((s64) ((double) COUNTER_SCALE * (counter)))

// Define the events we want to track with their event codes and umasks
static struct {
	u8 event_code;
	u8 umask;
	s64 koeff; // Coefficient for energy estimation
} tracked_events[] = {
	// CPU_CLK_UNHALTED.THREAD_P Thread cycles when thread is not in halt state
	{
		.event_code = 0x3c,
		.umask = 0x00,
		.koeff = SCALE_COUNTER(0.0021556045726281907),
	},
	// DTLB_STORE_MISSES.WALK_COMPLETED_4K Page walks completed due to a demand data store to a 4K page.
	{
		.event_code = 0x13,
		.umask = 0x02,
		.koeff = SCALE_COUNTER(-61.560037720824646),
	},
	// BR_MISP_RETIRED.ALL_BRANCHES All mispredicted branch instructions retired.
	{
		.event_code = 0xc5,
		.umask = 0x00,
		.koeff = SCALE_COUNTER(8.674131795501472),
	},
	// CPU_CLK_UNHALTED.C0_WAIT Core clocks when the thread is in the C0.1 or C0.2 or running a PAUSE in C0 ACPI state.
	{
		.event_code = 0xec,
		.umask = 0x70,
		.koeff = SCALE_COUNTER(-56.43560363241782),
	},
	// INT_MISC.UOP_DROPPING TMA slots where uops got dropped
	{
		.event_code = 0xad,
		.umask = 0x10,
		.koeff = SCALE_COUNTER(0.701297506177149),
	},
	// INST_RETIRED.ANY_P Number of instructions retired. General Counter - architectural event
	{
		.event_code = 0xc0,
		.umask = 0x00,
		.koeff = SCALE_COUNTER(0.00033669210675668637),
	},
	// EXE_ACTIVITY.1_PORTS_UTIL Cycles total of 1 uop is executed on all ports and Reservation Station was not empty.
	{
		.event_code = 0xa6,
		.umask = 0x02,
		.koeff = SCALE_COUNTER(0.00247793753839165),
	},
	// MEM_LOAD_RETIRED.L1_HIT Retired load instructions with L1 cache hits as data sources
	{
		.event_code = 0xd1,
		.umask = 0x01,
		.koeff = SCALE_COUNTER(-0.0010332474623950816),
	},
};

#define PACCT_TRACED_EVENT_COUNT ARRAY_SIZE(tracked_events)

struct proc_entry {
	struct proc_dir_entry *process_dir;
};

struct traced_task {
	struct list_head list;
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

	struct proc_entry proc_entry; // Associated file under proc
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

int powercap_init_caps(void);
void powercap_cleanup_caps(void);
void pacct_powercap_control_step(u64 pkg_power_mW);