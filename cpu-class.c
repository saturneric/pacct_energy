#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/cpumask.h>
#include <linux/cpufreq.h>

#include "cpu-class.h"

// simple way: differentiate power and efficiency cores based on their max frequency
// this requires that there are 2 kinds of max frequencies on the system

#define NCPU 256
static enum pacct_cpu_class pacct_cpu_classes[NCPU] = {
	0
}; // initialized to 0 = NONE

#define CPU_FREQ_UNINIT 0xffffffffu
int pacct_cpu_class_init(void)
{
	unsigned int freq_lo = CPU_FREQ_UNINIT;
	unsigned int freq_hi = CPU_FREQ_UNINIT;
	int cpu;
	for_each_online_cpu(cpu) { // TODO present CPU?
		unsigned int max_freq = cpufreq_quick_get_max(cpu);
		if (freq_lo == CPU_FREQ_UNINIT && freq_hi == CPU_FREQ_UNINIT) {
			// very first freq
			freq_lo = max_freq;
		} else if (freq_hi == CPU_FREQ_UNINIT) {
			if (max_freq < freq_lo) {
				freq_hi = freq_lo;
				freq_lo = max_freq;
			} else if (max_freq > freq_lo) {
				freq_hi = max_freq;
			}
		} else {
			if (max_freq != freq_lo && max_freq != freq_hi) {
				pr_err("more than 2 distinct frequencies\n");
				return -1;
			}
		}
	}
	// we now have 2 classes. Assign CPUs.
	for_each_online_cpu(cpu) {
		if (cpu < 0 || cpu >= NCPU) {
			pr_err("invalid CPU index\n");
			return -1;
		}
		pacct_cpu_classes[cpu] =
			(cpufreq_quick_get_max(cpu) == freq_lo) ?
				PACCT_CPU_CLASS_EFFICIENCY :
				PACCT_CPU_CLASS_PERFORMANCE;
	}

	return 0;
}

enum pacct_cpu_class pacct_cpu_class_get(int cpu)
{
	if (cpu < 0 || cpu >= NCPU) {
		return PACCT_CPU_CLASS_NONE;
	}
	return pacct_cpu_classes[cpu];
}
