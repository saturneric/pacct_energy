#ifndef PACCT_EVENTS_H
#define PACCT_EVENTS_H

#include <linux/types.h>

#include "cpu-class.h" // only for convenience

#define PACCT_NUM_EVENTS_PERFORMANCE 8
#define PACCT_NUM_EVENTS_EFFICIENCY 4
// conservative estimate for statically sized buffers
#define PACCT_NUM_EVENTS_MAX                                            \
	((PACCT_NUM_EVENTS_EFFICIENCY > PACCT_NUM_EVENTS_PERFORMANCE) ? \
		 PACCT_NUM_EVENTS_EFFICIENCY :                          \
		 PACCT_NUM_EVENTS_PERFORMANCE)
#define PACCT_NUM_EVENTS_ON_CLASS(cls)                                         \
	(((cls) == PACCT_CPU_CLASS_EFFICIENCY) ? PACCT_NUM_EVENTS_EFFICIENCY : \
						 PACCT_NUM_EVENTS_PERFORMANCE)
#define PACCT_NUM_EVENTS_ON_CPU(cpu) \
	PACCT_NUM_EVENTS_ON_CLASS(pacct_cpu_class_get(cpu))
struct pacct_event {
	u8 code;
	u8 umask;
	s64 coeff;
};
extern struct pacct_event pacct_events_efficiency[NUM_EVENTS_EFFICIENCY];
extern struct pacct_event pacct_events_performance[NUM_EVENTS_PERFORMANCE];

#endif
