#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/overflow.h>
#include <linux/timekeeping.h>

#include "model.h"
#include "errors.h"

#define CHECKED_ADD(res, a, b)                                          \
	do {                                                            \
		if (PACCT_ERROR_TRACE(model_overflow_add,               \
				      check_add_overflow(a, b, res))) { \
			return -1;                                      \
		}                                                       \
	} while (0)

#define CHECKED_MUL(res, a, b)                                          \
	do {                                                            \
		if (PACCT_ERROR_TRACE(model_overflow_mul,               \
				      check_mul_overflow(a, b, res))) { \
			return -1;                                      \
		}                                                       \
	} while (0)

int pacct_model_eval(const struct pacct_periodic_data *pd,
		     u64 *restrict energy_uj_out,
		     u64 *restrict power_wallclock_mW_out,
		     u64 *restrict power_cpu_mW_out)
{
	// set to default values for early return
	*energy_uj_out = 0;
	*power_wallclock_mW_out = 0;
	*power_cpu_mW_out = 0;

	s64 acc_efficiency = 0;
	s64 acc_performance = 0;

	s64 time_diff = (s64)ktime_get_ns() - (s64)pd->time_start_ns;
	if (PACCT_ERROR_TRACE(model_bad_time,
			      pd->time_start_ns == 0 || time_diff <= 0)) {
		return -1;
	}

	if (PACCT_ERROR_TRACE(
		    model_zero_runtime,
		    pd->time_efficiency_ns + pd->time_performance_ns == 0)) {
		// haven't been running at all.
		return 0;
	}

	s64 val;
	for (int i = 0; i < PACCT_NUM_EVENTS_EFFICIENCY; i++) {
		CHECKED_MUL(&val, pd->counter_diff_efficiency[i],
			    pacct_events_efficiency[i].coeff);
		CHECKED_ADD(&acc_efficiency, acc_efficiency, val);
	}
	for (int i = 0; i < PACCT_NUM_EVENTS_PERFORMANCE; i++) {
		CHECKED_MUL(&val, pd->counter_diff_performance[i],
			    pacct_events_performance[i].coeff);
		CHECKED_ADD(&acc_performance, acc_performance, val);
	}
	s64 acc;
	CHECKED_ADD(&acc, acc_efficiency, acc_performance);
	s64 energy_uj = acc / PACCT_COEFF_SCALE;
	PACCT_ERROR_TRACE(model_zero_energy, energy_uj == 0);
	if (PACCT_ERROR_TRACE(model_negative_energy, energy_uj < 0)) {
		// TODO better way?
		energy_uj = 0;
	}
	CHECKED_MUL(&acc, energy_uj, 1000000LL);
	s64 power_wallclock_mW = div64_s64(acc, time_diff);
	s64 time_on_cpu = pd->time_efficiency_ns + pd->time_performance_ns;
	s64 power_cpu_mW = 0;
	if (time_on_cpu > 0) {
		power_cpu_mW = div64_s64(acc, time_on_cpu);
	}
	*energy_uj_out = energy_uj;
	*power_wallclock_mW_out = power_wallclock_mW;
	*power_cpu_mW_out = power_cpu_mW;
	return 0;
}
