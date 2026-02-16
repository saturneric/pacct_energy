#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/cpu.h>
#include <linux/pm_qos.h>
#include <linux/cpufreq.h>

#include "pacct.h"

// CPU frequency scaling policy for P-cores
struct cap_policy {
	struct cpufreq_policy *policy;
	struct freq_qos_request max_req;
	bool req_added;
};

// Target package power in mW. The control loop will try to keep the total power
// of the package under this target by adjusting the CPU frequency caps.
static s32 target_mW = 30000;
module_param(target_mW, int, 0644);

// Hysteresis margin in mW to avoid too frequent adjustments of the CPU frequency caps.
static s32 hysteresis_mW = 800;
module_param(hysteresis_mW, int, 0644);

// Step size in kHz for adjusting the CPU frequency caps.
static s32 step_khz = 100000;
module_param(step_khz, int, 0644);

static struct cap_policy caps[NR_CPUS];
static int cap_cnt;
static s32 current_cap_khz = -1;

static int add_policy_cap_for_cpu(int cpu, s32 initial_max_khz)
{
	struct cpufreq_policy *pol;
	int ret;

	pol = cpufreq_cpu_get(cpu);
	if (!pol)
		return -ENODEV;

	// avoid duplicated policies
	for (int i = 0; i < cap_cnt; i++) {
		if (caps[i].policy == pol) {
			cpufreq_cpu_put(pol);
			return 0;
		}
	}

	caps[cap_cnt].policy = pol;
	ret = freq_qos_add_request(&pol->constraints, &caps[cap_cnt].max_req,
				   FREQ_QOS_MAX, initial_max_khz);
	if (ret < 0) {
		cpufreq_cpu_put(pol);
		return ret;
	}
	caps[cap_cnt].req_added = true;
	cap_cnt++;
	return 0;
}

static void update_policy_max(struct cap_policy *c, s32 max_khz)
{
	if (!c->req_added)
		return;

	// Make sure the new cap is within the CPU's supported frequency range
	if (max_khz < c->policy->cpuinfo.min_freq)
		max_khz = c->policy->cpuinfo.min_freq;
	if (max_khz > c->policy->cpuinfo.max_freq)
		max_khz = c->policy->cpuinfo.max_freq;

	freq_qos_update_request(&c->max_req, max_khz);
}

void powercap_cleanup_caps(void)
{
	for (int i = 0; i < cap_cnt; i++) {
		if (caps[i].req_added)
			freq_qos_remove_request(&caps[i].max_req);
		if (caps[i].policy)
			cpufreq_cpu_put(caps[i].policy);
	}
	cap_cnt = 0;
}

static void apply_cap_to_all(s32 cap_khz)
{
	// apply the new cpu frequency to all policies∑among the cores
	for (int i = 0; i < cap_cnt; i++)
		update_policy_max(&caps[i], cap_khz);
}

void pacct_powercap_control_step(u64 pkg_power_mW)
{
	if (current_cap_khz < 0) {
		current_cap_khz = caps[0].policy->cpuinfo.max_freq;
		apply_cap_to_all(current_cap_khz);
		return;
	}

	// If the package power is above the target + hysteresis, reduce the CPU
	// frequency cap by one step. If the package power is below the target
	// hysteresis, increase the CPU frequency cap by one step.
	if (pkg_power_mW > target_mW + hysteresis_mW) {
		current_cap_khz -= step_khz;
		apply_cap_to_all(current_cap_khz);
	} else if (pkg_power_mW < target_mW - hysteresis_mW) {
		current_cap_khz += step_khz;
		apply_cap_to_all(current_cap_khz);
	}
}

int powercap_init_caps(void)
{
	int cpu, ret;

	cap_cnt = 0;
	current_cap_khz = -1;

	for_each_online_cpu(cpu) {
		ret = add_policy_cap_for_cpu(cpu, INT_MAX);
		if (ret && ret != -ENODEV) {
			pr_err("add_policy_cap_for_cpu cpu=%d ret=%d\n", cpu,
			       ret);
			powercap_cleanup_caps();
			return ret;
		}
	}

	if (cap_cnt == 0) {
		pr_err("no cpufreq policy found, cannot powercap\n");
		return -ENODEV;
	}

	// Set the initial cap to the maximum of the current max frequencies of all
	// policies, so that we don't unnecessarily limit the frequency at the
	// beginning.
	current_cap_khz = caps[0].policy->cpuinfo.max_freq;
	for (int i = 1; i < cap_cnt; i++) {
		s32 m = caps[i].policy->cpuinfo.max_freq;
		if (m > current_cap_khz)
			current_cap_khz = m;
	}

	// Apply the initial cap to all policies. This is important to do before we
	// start the control loop to ensure that we have a known starting point for
	// the CPU frequency caps.
	apply_cap_to_all(current_cap_khz);

	pr_info("powercap: policies=%d initial_cap=%d kHz target=%d mW\n",
		cap_cnt, current_cap_khz, target_mW);

	return 0;
}