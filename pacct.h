#pragma once

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>

#define PACCT_TRACED_EVENT_COUNT 8

struct periodic_data {
	u64 time_start_ns;
	u64 time_efficiency_ns;
	u64 time_performance_ns;
	u64 counter_diff_efficiency[PACCT_TRACED_EVENT_COUNT];
	u64 counter_diff_performance[PACCT_TRACED_EVENT_COUNT];
};
int calculate_model(const struct periodic_data *pd, u64 *restrict energy_uj_out,
		    u64 *restrict power_wallclock_mW_out,
		    u64 *restrict power_cpu_mW_out);

struct traced_task {
	struct list_head list;
	struct list_head retire_node; // Node for the retiring_traced_tasks list
	struct kref ref_count; // Reference count for this traced task entry
	pid_t pid;
	bool ready; // If the task is ready to be sampled
	bool retiring; // Flag to indicate if this task is being retired and should not be sampled anymore
	struct perf_event *event[PACCT_TRACED_EVENT_COUNT];

	// data for a single scheduling
	bool running;
	u64 run_start_ns;
	u64 counter_start[PACCT_TRACED_EVENT_COUNT];

	// data that is periodically calculated
	spinlock_t periodic_lock; // TODO verify that this lock is used everywhere
	struct periodic_data periodic_data;
	u64 energy_uj; // total energy used by this process
	u64 power_wallclock_mW;
	u64 power_cpu_mW;

	char comm[TASK_COMM_LEN];

	struct proc_dir_entry *process_dir; // Associated file under proc
};

extern struct stats {
	s64 energy;
	s64 power;
	s64 energy_rapl;
	s64 power_rapl;
} global_stats;

struct traced_task *new_traced_task(pid_t pid);
void release_traced_task(struct kref *kref);
int setup_traced_task(struct traced_task *entry);
struct traced_task *get_or_create_traced_task(pid_t pid, const char *comm,
					      bool create);

void queue_pacct_setup_work(void);
void queue_pacct_retire_work(void);
void queue_pacct_scan_tasks(void);
void pacct_start_energy_estimator(void);
void pacct_stop_energy_estimator(void);

struct task_struct *get_task_by_pid(pid_t pid);
u64 read_event_count(struct perf_event *ev);
u64 read_event_count_sleepable(struct perf_event *ev);

int powercap_init_caps(void);
void powercap_cleanup_caps(void);
void pacct_powercap_control_step(u64 pkg_power_mW);
