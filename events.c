#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include "events.h"

// TODO real values

struct pacct_event events_efficiency[NUM_EVENTS_EFFICIENCY] = {
	// insn retired any
	{ .code = 0x00, .umask = 0x01 },
	// mem scheduler block
	{ .code = 0x04, .umask = 0x07 },
	// L2 request all
	{ .code = 0x24, .umask = 0x00 },
	// L2 request miss
	{ .code = 0x24, .umask = 0x01 },
};

struct pacct_event events_performance[NUM_EVENTS_PERFORMANCE] = {
	// insn retired any
	{ .code = 0x00, .umask = 0x01 },
	// itlb misses
	{ .code = 0x11, .umask = 0x0e },
	// dtlb load misses
	{ .code = 0x12, .umask = 0x0e },
	// dtlb store misses
	{ .code = 0x13, .umask = 0x0e },
};
