#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/perf_event.h>
#include "events.h"
#include "cpu-class.h"
#include "counters.h"

#define NUM_CPUS 20 // TODO?
static struct perf_event *pacct_perf_events[NUM_CPUS][NUM_EVENTS_MAX] = { 0 };

static void my_overflow_handler(struct perf_event *pev,
				struct perf_sample_data *sample,
				struct pt_regs *regs)
{
	pr_err("overflow handler called for perf event %p\n", pev);
}

#define EVENT_CONFIG(ev) ((u64)(ev).code | ((u64)(ev).umask << 8))

/*
 * Creates a new raw counter on specified CPU.
 * Returns the counter if successful, `NULL` otherwise.
 */
static struct perf_event *install_counter(struct pacct_event ev, int cpu)
{
	struct perf_event_attr attr = { 0 };
	attr.config = EVENT_CONFIG(ev);
	attr.type =
		pacct_cpu_class_get(cpu) == PACCT_CPU_CLASS_EFFICIENCY ? 10 : 4;
	attr.size = sizeof(attr);
	attr.pinned = 1;
	struct perf_event *perf_event = perf_event_create_kernel_counter(
		&attr, cpu, NULL, my_overflow_handler, NULL);
	if (IS_ERR_VALUE(perf_event)) {
		pr_err("could not install counter (c=%02x,u=%02x) on core %d\n",
		       ev.code, ev.umask, cpu);
		return NULL;
	}
	return perf_event;
}

int pacct_counters_install(void)
{
	int cpu;
	// TODO for each possible CPU? What if one comes online later?
	for_each_online_cpu(cpu) {
		switch (pacct_cpu_class_get(cpu)) {
		case PACCT_CPU_CLASS_EFFICIENCY:
			for (int i = 0; i < NUM_EVENTS_EFFICIENCY; i++) {
				pacct_perf_events[cpu][i] = install_counter(
					events_efficiency[i], cpu);
				if (pacct_perf_events[cpu][i] == NULL) {
					goto err;
				}
			}
			break;
		case PACCT_CPU_CLASS_PERFORMANCE:
			for (int i = 0; i < NUM_EVENTS_PERFORMANCE; i++) {
				pacct_perf_events[cpu][i] = install_counter(
					events_performance[i], cpu);
				if (pacct_perf_events[cpu][i] == NULL) {
					goto err;
				}
			}
			break;
		}
	}
	return 0;
err:
	pacct_counters_uninstall();
	return -1;
}

static void uninstall_counter(struct perf_event *perf_event)
{
	if (perf_event != NULL) {
		int ret = perf_event_release_kernel(perf_event);
		if (ret) {
			pr_err("error in perf_event_release_kernel\n");
		}
	}
}

void pacct_counters_uninstall(void)
{
	pr_info("uninstalling counters\n");
	for (int cpu = 0; cpu < NUM_CPUS; cpu++) {
		for (int i = 0; i < NUM_EVENTS_MAX; i++) {
			uninstall_counter(pacct_perf_events[cpu][i]);
		}
	}
}

u64 pacct_counter_read_local(unsigned int counter)
{
	if (PACCT_ERROR_TRACE(counter_bad_read, counter >= NUM_EVENTS_MAX)) {
		return 0;
	}
	int me = get_cpu();
	struct perf_event *pev = pacct_perf_events[me][counter];
	u64 value = 0, enabled, running;
	if (pev != NULL) {
		int ret =
			perf_event_read_local(pev, &value, &enabled, &running);
		if (PACCT_ERROR_TRACE(counter_bad_read, ret)) {
			put_cpu();
			return 0;
		}
		PACCT_ERROR_TRACE(counter_multiplexed, enabled != running);
	}
	put_cpu();
	return value;
}
