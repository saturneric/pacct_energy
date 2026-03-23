#include "pacct.h"

#include <asm/msr.h>
#include <asm/msr-index.h>

#define MSR_IA32_HW_FEEDBACK_CHAR 0x17d2

#define PACCT_ITD_SAMPLE_INTERVAL_MS 1

union hfi_thread_feedback_char_msr {
	struct {
		u64 classid : 8;
		u64 __reserved : 55;
		u64 valid : 1;
	} split;
	u64 full;
};

struct pacct_cpu_sampler {
	struct hrtimer timer;
};

union cpuid6_ecx {
	struct {
		u32 dont_care0 : 8;
		u32 nr_classes : 8;
		u32 dont_care1 : 16;
	} split;
	u32 full;
};

// Number of ITD classes supported by the CPU, read from CPUID
static u64 n_itd_classes;

static DEFINE_PER_CPU(struct pacct_cpu_sampler, pacct_samplers);

/**
 * intel_hfi_read_classid() - Read the current classid
 * @classid:	Variable to which the classid will be written.
 *
 * Read the classification that Intel Thread Director has produced when this
 * function is called. Thread classification must be enabled before calling
 * this function.
 *
 * Return: 0 if the produced classification is valid. Error otherwise.
 */
int intel_hfi_read_classid(u8 *classid)
{
	union hfi_thread_feedback_char_msr msr;

	/* We should not be here if ITD is not supported. */
	if (!cpu_feature_enabled(X86_FEATURE_ITD)) {
		pr_warn_once(
			"task classification requested but not supported!");
		return -ENODEV;
	}

	rdmsrl(MSR_IA32_HW_FEEDBACK_CHAR, msr.full);
	if (!msr.split.valid)
		return -EINVAL;

	*classid = msr.split.classid;
	return 0;
}

void pacct_sample_current_itd(void)
{
	struct traced_task *e;
	u8 classid;
	int ret;

	e = get_or_create_traced_task(current->pid, NULL, false);
	if (!e)
		return;

	ret = intel_hfi_read_classid(&classid);
	if (!ret) {
		// Add 1 to classid so that 0 can be reserved for unclassified tasks
		e->itd_classid_count[classid + 1]++;
	} else {
		e->itd_classid_count[0]++;
	}

	e->itd_sample_count++;

	// After enough samples, determine the most common classid for this task and
	// use that as the classid for the task. This is to reduce the noise in
	// classification and get a more stable classid for each task
	if (e->itd_sample_count % 100 == 0) {
		u64 max_count = 0;
		u64 max_classid = 0;
		for (int i = 0; i < n_itd_classes; i++) {
			if (e->itd_classid_count[i] > max_count) {
				max_count = e->itd_classid_count[i];
				max_classid = i;
			}
			e->itd_classid_count[i] = 0;
		}
		e->itd_classid = max_classid;
	}

	// Reset the sample count to avoid overflow and to allow the classid to be
	// updated over time if the task's behavior changes
	if (e->itd_sample_count > 1000) {
		e->itd_sample_count = 0;
		for (int i = 0; i < n_itd_classes; i++) {
			e->itd_classid_count[i] = 0;
		}
	}

	kref_put(&e->ref_count, release_traced_task);
}

static enum hrtimer_restart pacct_sample_timer_fn(struct hrtimer *timer)
{
	// Sample the current ITD classid for the current task
	pacct_sample_current_itd();

	// Schedule the next sampling after a fixed interval
	hrtimer_forward_now(timer, ms_to_ktime(PACCT_ITD_SAMPLE_INTERVAL_MS));
	return HRTIMER_RESTART;
}

static void pacct_start_sampler_cpu(void *info)
{
	struct pacct_cpu_sampler *s = this_cpu_ptr(&pacct_samplers);

	hrtimer_setup(&s->timer, pacct_sample_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_PINNED);
	hrtimer_start(&s->timer, ms_to_ktime(PACCT_ITD_SAMPLE_INTERVAL_MS),
		      HRTIMER_MODE_REL_PINNED);
}

static void pacct_stop_sampler_cpu(void *info)
{
	struct pacct_cpu_sampler *s = this_cpu_ptr(&pacct_samplers);
	hrtimer_cancel(&s->timer);
}

static void pacct_enable_itd_per_cpu(void *ignore)
{
	u64 msr_val;

	if (cpu_feature_enabled(X86_FEATURE_ITD)) {
		msr_val = HW_FEEDBACK_THREAD_CONFIG_ENABLE_BIT;
		wrmsrl(MSR_IA32_HW_FEEDBACK_THREAD_CONFIG, msr_val);
	}

	/*
	 * Enable the hardware feedback interface and never disable it. See
	 * comment on programming the address of the table.
	 */
	rdmsrl(MSR_IA32_HW_FEEDBACK_CONFIG, msr_val);
	msr_val |= HW_FEEDBACK_CONFIG_HFI_ENABLE_BIT;

	if (cpu_feature_enabled(X86_FEATURE_ITD))
		msr_val |= HW_FEEDBACK_CONFIG_ITD_ENABLE_BIT;

	wrmsrl(MSR_IA32_HW_FEEDBACK_CONFIG, msr_val);
}

void pacct_init_itd()
{
	if (!cpu_feature_enabled(X86_FEATURE_ITD))
		return;

	pr_info("enabling intel thread director support on all cpus\n");

	// Enable ITD support on all CPUs by writing to the appropriate MSR
	on_each_cpu(pacct_enable_itd_per_cpu, NULL, 1);

	// Read the number of ITD classes supported by the CPU
	union cpuid6_ecx ecx;
	ecx.full = cpuid_ecx(CPUID_HFI_LEAF);
	n_itd_classes = ecx.split.nr_classes;

	pr_info("cpu supports %llu ITD classes\n", n_itd_classes);

	// Start the periodic sampling of ITD classid for the current task on each CPU
	on_each_cpu(pacct_start_sampler_cpu, NULL, 1);
}

void pacct_exit_itd()
{
	on_each_cpu(pacct_stop_sampler_cpu, NULL, 1);
}