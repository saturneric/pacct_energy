#ifndef PACCT_MODEL_H
#define PACCT_MODEL_H

#include "traced_task.h"

/*
 * Calculates model on the given periodic data.
 */
int calculate_model(const struct periodic_data *pd, u64 *restrict energy_uj_out,
		    u64 *restrict power_wallclock_mW_out,
		    u64 *restrict power_cpu_mW_out);

#endif
