#ifndef PACCT_MODEL_H
#define PACCT_MODEL_H

#include "traced-task.h"

/*
 * Calculates model on the given periodic data.
 */
int pacct_model_eval(const struct pacct_periodic_data *pd,
		     u64 *restrict energy_uj_out,
		     u64 *restrict power_wallclock_mW_out,
		     u64 *restrict power_cpu_mW_out);

#endif
