#ifndef PACCT_COUNTERS_H
#define PACCT_COUNTERS_H

#include <linux/perf_event.h>

#include "events.h"

#define NUM_CPUS 20 // TODO

int pacct_counters_install(void);
void pacct_counters_uninstall(void);

extern struct perf_event *pacct_perf_events[NUM_CPUS][NUM_EVENTS_MAX];

#endif
