#ifndef PACCT_ERRORS_H
#define PACCT_ERRORS_H

// TODO these should be atomic probably
struct pacct_errors {
	u64 model_bad_time;
	u64 model_overflow_mul;
	u64 model_overflow_add;
	u64 model_negative_energy;
	u64 model_zero_energy;
	u64 sched_switch_prev_no_traced_task;
	u64 sched_switch_prev_not_ready;
	u64 sched_switch_prev_not_running;
	u64 sched_switch_prev_invalid_cpu_class;
	u64 sched_switch_prev_counter_got_smaller;
	u64 sched_switch_next_no_traced_task;
	u64 sched_switch_next_not_ready;
	u64 sched_switch_next_already_running;
};
extern struct pacct_errors pacct_errors;

void pacct_error_report(void);

#endif
