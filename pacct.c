#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include "pacct.h"
#include "proc.h"
#include "errors.h"

#include <linux/perf_event.h>
#include <linux/timekeeping.h>

struct pacct_errors pacct_errors = { 0 };

#define COUNTER_SCALE 100000000 //For fixed point arithmetic
#define SCALE_COUNTER(counter) ((s64)((double)COUNTER_SCALE * (counter)))

// Define the events we want to track with their event codes and umasks
struct pacct_event {
	u8 event_code;
	u8 umask;
	s64 coeff_efficiency;
	s64 coeff_performance;
};
struct pacct_event pacct_events[PACCT_TRACED_EVENT_COUNT] = {
	{
		// Thread cycles when thread is not in halt state
		.event_code = 0x3c,
		.umask = 0x00,
		.coeff_efficiency = SCALE_COUNTER(0.0025449054478549338),
		.coeff_performance = SCALE_COUNTER(0.0025449054478549338),
	},
	{
		// Number of instructions retired. General Counter - architectural event
		.event_code = 0xc0,
		.umask = 0x00,
		.coeff_efficiency = SCALE_COUNTER(-0.002922289591384071),
		.coeff_performance = SCALE_COUNTER(-0.002922289591384071),
	},
	{
		// All branch instructions retired.
		.event_code = 0xc4,
		.umask = 0x00,
		.coeff_efficiency = SCALE_COUNTER(0.001034537512176854),
		.coeff_performance = SCALE_COUNTER(0.001034537512176854),
	},
	{
		// All mispredicted branch instructions retired.
		.event_code = 0xc5,
		.umask = 0x00,
		.coeff_efficiency = SCALE_COUNTER(-0.9953297240136587),
		.coeff_performance = SCALE_COUNTER(-0.9953297240136587),
	},
	{
		// Number of instructions retired. Fixed Counter - architectural event
		.event_code = 0x00,
		.umask = 0x01,
		.coeff_efficiency = SCALE_COUNTER(0.003169241994353939),
		.coeff_performance = SCALE_COUNTER(0.003169241994353939),
	},
	{
		// Cycles where at least 1 outstanding demand data read request is pending.
		.event_code = 0x20,
		.umask = 0x01,
		.coeff_efficiency = SCALE_COUNTER(0.0008327786226836792),
		.coeff_performance = SCALE_COUNTER(0.0008327786226836792),
	},
	{
		// Demand Data Read requests sent to uncore
		.event_code = 0x21,
		.umask = 0x01,
		.coeff_efficiency = SCALE_COUNTER(0.012861678862859853),
		.coeff_performance = SCALE_COUNTER(0.012861678862859853),
	},
	{
		// Non-modified cache lines that are silently dropped by L2 cache.
		.event_code = 0x26,
		.umask = 0x01,
		.coeff_efficiency = SCALE_COUNTER(-0.1042917907300562),
		.coeff_performance = SCALE_COUNTER(-0.1042917907300562),
	},
};

extern spinlock_t traced_tasks_lock;
extern struct list_head traced_tasks;

struct traced_task *new_traced_task(pid_t pid)
{
	struct traced_task *entry;

	// Allocate and initialize a new traced_task entry
	// Use GFP_ATOMIC since this will be called from an atomic context
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		pr_err("Failed to allocate memory for traced_task\n");
		return NULL;
	}
	// all fields are 0-initialized here; only override the ones != 0
	kref_init(&entry->ref_count);
	entry->pid = pid;
	spin_lock_init(&entry->periodic_lock);
	return entry;
}

void release_traced_task(struct kref *kref)
{
	struct traced_task *entry =
		container_of(kref, struct traced_task, ref_count);

	// Disable and release all events for this traced task
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (entry->event[i] && !IS_ERR(entry->event[i])) {
			perf_event_disable(entry->event[i]);
			perf_event_release_kernel(entry->event[i]);
		}
	}
	freeProcFile(entry);
	// Free the traced_task structure itself
	kfree(entry);
}

static int setup_task_counter(pid_t pid, struct perf_event **event,
			      u8 event_code, u8 umask)
{
	int ret;
	struct perf_event_attr attr;
	struct task_struct *t;
	u64 raw = (u64)event_code | ((u64)umask << 8);

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_RAW;
	attr.config = raw;
	attr.size = sizeof(attr);

	attr.disabled = 1;
	attr.exclude_kernel = 0;
	attr.exclude_user = 0;
	attr.exclude_hv = 0;

	t = get_task_by_pid(pid);

	if (!t) {
		ret = -ESRCH;
		goto err;
	}

	*event = perf_event_create_kernel_counter(&attr, -1, t, NULL, NULL);
	put_task_struct(t);

	if (IS_ERR(*event)) {
		pr_err("Failed to create perf event for PID %d: %ld\n", pid,
		       PTR_ERR(*event));
		ret = -1;
		goto err;
	}

	// do not enable events here TODO
	perf_event_enable(*event);
	return 0;

err:
	return ret;
}

int setup_traced_task(struct traced_task *entry)
{
	int ret;
	kref_get(&entry->ref_count);

	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		if (entry->event[i] && !IS_ERR(entry->event[i]))
			continue; // Counter already set up for this event

		ret = setup_task_counter(entry->pid, &entry->event[i],
					 pacct_events[i].event_code,
					 pacct_events[i].umask);
		if (ret < 0) {
			pr_err("Failed to set up counter for PID %d event code 0x%02x "
			       "umask 0x%02x ret %d\n",
			       entry->pid, pacct_events[i].event_code,
			       pacct_events[i].umask, ret);
			goto err;
		}
	}
	setup_proc_file(entry);
	kref_put(&entry->ref_count, release_traced_task);
	return 0;
err:
	kref_put(&entry->ref_count, release_traced_task);
	return ret;
}

struct traced_task *get_or_create_traced_task(pid_t pid, const char *comm,
					      bool create)
{
	struct traced_task *entry;

	spin_lock(&traced_tasks_lock);
	list_for_each_entry(entry, &traced_tasks, list) {
		if (entry->pid == pid) {
			// Found an existing entry for this PID, increment refcount and return it
			goto out;
		}
	}

	if (!create) {
		// No existing entry found and creation not allowed, return NULL
		entry = NULL;
		goto err;
	}

	// No existing entry found, create a new one
	entry = new_traced_task(pid);
	if (!entry) {
		pr_err("Failed to create traced task for PID %d\n", pid);
		goto err;
	}

	if (comm) {
		strncpy(entry->comm, comm, TASK_COMM_LEN - 1);
		entry->comm[TASK_COMM_LEN - 1] = '\0';
	}

	list_add(&entry->list, &traced_tasks);

out:
	// Increment refcount for the new entry
	kref_get(&entry->ref_count);
err:
	spin_unlock(&traced_tasks_lock);
	return entry;
}

#define CHECKED_ADD(res, a, b)                                          \
	do {                                                            \
		if (PACCT_ERROR_TRACE(model_overflow_add,               \
				      check_add_overflow(a, b, res))) { \
			goto overflow;                                  \
		}                                                       \
	} while (0)

#define CHECKED_MUL(res, a, b)                                          \
	do {                                                            \
		if (PACCT_ERROR_TRACE(model_overflow_mul,               \
				      check_mul_overflow(a, b, res))) { \
			goto overflow;                                  \
		}                                                       \
	} while (0)

// calculates model energy and power in timeframe given in `periodic_data`.
int calculate_model(const struct periodic_data *pd, u64 *restrict energy_uj_out,
		    u64 *restrict power_wallclock_mW_out,
		    u64 *restrict power_cpu_mW_out)
{
	s64 acc_efficiency = 0;
	s64 acc_performance = 0;

	s64 time_diff = (s64)ktime_get_ns() - (s64)pd->time_start_ns;
	if (PACCT_ERROR_TRACE(model_bad_time, time_diff <= 0)) {
		goto overflow;
	}

	// TODO IS_ERR() on the events, and NULL check?
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		s64 val;
		CHECKED_MUL(&val, pd->counter_diff_efficiency[i],
			    pacct_events[i].coeff_efficiency);
		CHECKED_ADD(&acc_efficiency, acc_efficiency, val);
		CHECKED_MUL(&val, pd->counter_diff_performance[i],
			    pacct_events[i].coeff_performance);
		CHECKED_ADD(&acc_performance, acc_performance, val);
	}
	s64 acc;
	CHECKED_ADD(&acc, acc_efficiency, acc_performance);
	s64 energy_uj = acc / COUNTER_SCALE;
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
overflow:
	*energy_uj_out =
		0; // TODO this needs to be summed on the total, other values can be used directly.
	*power_wallclock_mW_out = 0;
	*power_cpu_mW_out = 0;
	return -1;
}

#define PACCT_ERROR_TRANSFORM(name)                                      \
	pr_info("  " #name " = %llu/%llu\n", pacct_errors.name.positive, \
		pacct_errors.name.positive + pacct_errors.name.negative);
void pacct_error_report(void)
{
	pr_info("pacct_errors {\n");
	PACCT_ERRORS
	pr_info("}\n");
}
#undef PACCT_ERROR_TRANSFORM

int pacct_error_trace(struct pacct_error_trace *tr, int cond)
{
	if (cond) {
		tr->positive++;
	} else {
		tr->negative++;
	}
	return cond;
}
