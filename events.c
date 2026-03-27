#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include "events.h"

// TODO real values
struct pacct_event pacct_events_efficiency[PACCT_NUM_EVENTS_EFFICIENCY] = {
	// insn retired any
	{ .code = 0x00, .umask = 0x01, .coeff = 0 },
	// mem scheduler block
	{ .code = 0x04, .umask = 0x07, .coeff = 0 },
	// L2 request all
	{ .code = 0x24, .umask = 0x00, .coeff = 0 },
	// L2 request miss
	{ .code = 0x24, .umask = 0x01, .coeff = 0 },
};

#define COUNTER_SCALE 100000000
#define SCALE_COUNTER(counter) ((s64)((double)COUNTER_SCALE * (counter)))

struct pacct_event pacct_events_performance[PACCT_NUM_EVENTS_PERFORMANCE] = {
	{
		// Thread cycles when thread is not in halt state.
		.code = 0x3c,
		.umask = 0x00,
		.coeff = SCALE_COUNTER(0.0025449054478549338),
	},
	{
		// Number of instructions retired.
		.code = 0xc0,
		.umask = 0x00,
		.coeff = SCALE_COUNTER(-0.002922289591384071),
	},
	{
		// All branch instructions retired.
		.code = 0xc4,
		.umask = 0x00,
		.coeff = SCALE_COUNTER(0.001034537512176854),
	},
	{
		// All mispredicted branch instructions retired.
		.code = 0xc5,
		.umask = 0x00,
		.coeff = SCALE_COUNTER(-0.9953297240136587),
	},
	{
		// Number of instructions retired.
		.code = 0x00,
		.umask = 0x01,
		.coeff = SCALE_COUNTER(0.003169241994353939),
	},
	{
		// Cycles where at least 1 outstanding demand data read request is pending.
		.code = 0x20,
		.umask = 0x01,
		.coeff = SCALE_COUNTER(0.0008327786226836792),
		.coeff_performance = SCALE_COUNTER(0.0008327786226836792),
	},
	{
		// Demand Data Read requests sent to uncore
		.code = 0x21,
		.umask = 0x01,
		.coeff = SCALE_COUNTER(0.012861678862859853),
	},
	{
		// Non-modified cache lines that are silently dropped by L2 cache.
		.code = 0x26,
		.umask = 0x01,
		.coeff = SCALE_COUNTER(-0.1042917907300562),
	},
};
