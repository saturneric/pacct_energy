#ifndef PACCT_EVENTS_H
#define PACCT_EVENTS_H

#include <linux/types.h>

#define NUM_EVENTS_PERFORMANCE 4
#define NUM_EVENTS_EFFICIENCY 4
#define NUM_EVENTS_MAX                                      \
	((NUM_EVENTS_EFFICIENCY > NUM_EVENTS_PERFORMANCE) ? \
		 NUM_EVENTS_EFFICIENCY :                    \
		 NUM_EVENTS_PERFORMANCE)
struct pacct_event {
	u8 code;
	u8 umask;
	// TODO coefficients
};
extern struct pacct_event events_efficiency[NUM_EVENTS_EFFICIENCY];
extern struct pacct_event events_performance[NUM_EVENTS_PERFORMANCE];

#endif
