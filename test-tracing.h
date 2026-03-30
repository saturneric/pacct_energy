#ifndef PACCT_TEST_TRACING_H
#define PACCT_TEST_TRACING_H

struct pacct_test_trace {
	struct task_struct *ts;
	struct pacct_traced_task *tt;
};
#define PACCT_TEST_TRACING_NUM 15
extern int pacct_test_traces_num; // TODO atomic
extern struct pacct_test_trace pacct_test_traces[PACCT_TEST_TRACING_NUM];

#endif
