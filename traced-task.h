#ifndef TRACED_TASK_H
#define TRACED_TASK_H

#include "events.h"

extern struct list_head pacct_traced_tasks;
extern struct list_head pacct_retiring_traced_tasks;
extern spinlock_t pacct_traced_tasks_lock;

struct pacct_periodic_data {
	u64 time_start_ns;
	u64 time_efficiency_ns;
	u64 time_performance_ns;
	u64 counter_diff_efficiency[PACCT_NUM_EVENTS_EFFICIENCY];
	u64 counter_diff_performance[PACCT_NUM_EVENTS_PERFORMANCE];
};

struct pacct_traced_task {
	struct list_head list;
	struct list_head retire_node;
	struct kref ref_count;
	pid_t pid;

	bool ready;
	bool retiring;

	// data for a single scheduling
	bool running;
	int current_cpu; // for sanity checking
	u64 run_start_ns;
	u64 counter_start[PACCT_NUM_EVENTS_MAX];

	// data that is periodically calculated
	spinlock_t periodic_lock; // TODO verify that this lock is used everywhere
	struct pacct_periodic_data periodic_data;
	u64 energy_uj; // total energy used by this process
	u64 power_wallclock_mW;
	u64 power_cpu_mW;

	struct proc_dir_entry *process_dir; // Associated file under proc
};

/*
 * Initializes global data structures for task tracing.
 */
void pacct_traced_tasks_init(void);
/*
 * Moves all currently traced tasks to the retiring list for cleanup.
 */
void pacct_traced_tasks_clean(void);

/*
 * Allocates memory for a new `traced_task`.
 * Can be called from atomic context.
 * Doesn't do full initialization; you must call `traced_task_setup`
 * sometime after from a non-atomic context.
 * Only use this struct once `ready` is set to `true`.
 *
 * May return `NULL` if memory is exhausted.
 */
struct pacct_traced_task *pacct_traced_task_create(pid_t pid);
/*
 * Non-atomic setup.
 * Once done, sets `ready` to `true`.
 */
int pacct_traced_task_setup(struct pacct_traced_task *tt);
/*
 * Attempts to fetch a task from the task list, by `pid`.
 * Increments the refcount of the found task.
 * Returns `NULL` if not found.
 */
struct pacct_traced_task *traced_task_get(pid_t pid);
/*
 * Releases a traced task after all references have gone.
 */
void pacct_traced_task_release(struct kref *kref);

// --- global stats ---

struct pacct_stats {
	s64 energy;
	s64 power;
	s64 energy_rapl;
	s64 power_rapl;
};
extern struct pacct_stats global_stats;
#endif
