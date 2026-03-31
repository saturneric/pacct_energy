#ifndef PACCT_TRACEPOINTS_H
#define PACCT_TRACEPOINTS_H

int pacct_tracepoints_register(void);
void pacct_tracepoints_unregister(void);

int pacct_setup_cpu_timers(u64 interval_ms);
void pacct_cleanup_cpu_timers(void);

#endif
