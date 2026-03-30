/*
 * Simple condition tracking functionality.
 * If you want to track a condition, add a member for the `PACCT_ERRORS` below,
 * then use `PACCT_ERROR_TRACE` wherever you evaluate the condition.
 *
 * Example for tracing `answer == 42`:
 * ```c
 * // add this to PACCT_ERRORS below
 * PACCT_ERROR_TRANSFORM(found_the_answer)
 *
 * // add this in your actual code
 * if (PACCT_ERROR_TRACE(found_the_answer, answer == 42) {
 * 	// success, answer is 42
 * } else {
 * 	// fail
 * }
 * ```
 */
#ifndef PACCT_ERRORS_H
#define PACCT_ERRORS_H

#include <linux/types.h>

// TODO these should be atomic probably
// TODO could make this branchless by using array with index 0, 1
struct pacct_error_trace {
	u64 positive;
	u64 negative;
};
int pacct_error_trace(struct pacct_error_trace *tr, int cond);
#define PACCT_ERROR_TRACE(name, cond) \
	pacct_error_trace(&pacct_errors.name, cond)

#define PACCT_ERRORS                                                 \
	PACCT_ERROR_TRANSFORM(model_bad_time)                        \
	PACCT_ERROR_TRANSFORM(model_overflow_mul)                    \
	PACCT_ERROR_TRANSFORM(model_overflow_add)                    \
	PACCT_ERROR_TRANSFORM(model_negative_energy)                 \
	PACCT_ERROR_TRANSFORM(model_zero_energy)                     \
	PACCT_ERROR_TRANSFORM(sched_switch_prev_no_traced_task)      \
	PACCT_ERROR_TRANSFORM(sched_switch_prev_not_running)         \
	PACCT_ERROR_TRANSFORM(sched_switch_prev_wrong_cpu)           \
	PACCT_ERROR_TRANSFORM(sched_switch_prev_counter_got_smaller) \
	PACCT_ERROR_TRANSFORM(sched_switch_next_no_traced_task)      \
	PACCT_ERROR_TRANSFORM(sched_switch_next_already_running)     \
	PACCT_ERROR_TRANSFORM(sched_exit_no_traced_task)             \
	PACCT_ERROR_TRANSFORM(read_event_count_run_ena_mismatch)     \
	PACCT_ERROR_TRANSFORM(read_event_count_run_ena_mismatch_big) \
	PACCT_ERROR_TRANSFORM(counter_bad_read)                      \
	PACCT_ERROR_TRANSFORM(counter_multiplexed)

#define PACCT_ERROR_TRANSFORM(name) struct pacct_error_trace name;
struct pacct_errors {
	PACCT_ERRORS
};
#undef PACCT_ERROR_TRANSFORM
extern struct pacct_errors pacct_errors;

/*
 * Reports all errors using `pr_info`.
 */
void pacct_error_report(void);

#endif
