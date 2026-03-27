#ifndef PACCT_COUNTERS_H
#define PACCT_COUNTERS_H

#include <linux/perf_event.h>

#include "events.h"

int pacct_counters_install(void);
void pacct_counters_uninstall(void);

/*
 * Reads an event from the current CPU core.
 * You should be in atomic context when you call this!
 *
 * @counter: counter value (range 0..=PACCT_NUM_EVENTS_MAX).
 * 	If there are less counters installed on this core, returns 0.
 */
u64 pacct_counter_read_local(unsigned int counter);

#endif
