#ifndef PACCT_WQ_H
#define PACCT_WQ_H

/*
 * Puts a task in the workqueue which goes through the traced tasks list and
 * initializes tasks that are not ready.
 */
void pacct_queue_setup_work(void);

/*
 * Puts a task in the workqueue which deletes tasks from the retiring list,
 * decreasing their refcount and deallocating them.
 */
void pacct_queue_retire_work(void);

// TODO simply do this at module initialization?
void pacct_queue_scan_tasks(void);

// --- periodic tasks ---

void pacct_queue_energy_estimator_start(void);
void pacct_queue_energy_estimator_stop(void);

#endif
