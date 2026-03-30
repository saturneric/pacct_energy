#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/tracepoint.h>
#include <linux/smp.h>

#include "proc.h"
#include "cpu-class.h"
#include "errors.h"
#include "tracepoints.h"
#include "traced-task.h"
#include "counters.h"
#include "wq.h"

MODULE_AUTHOR("pm3");
MODULE_DESCRIPTION("Process Energy Accounting Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

// RAPL things
u64 last_pkg_raw, last_ns;

// Global statistics
struct pacct_stats global_stats = {};

static int __init pacct_energy_init(void)
{
	int ret;

	pr_info("pacct_energy init\n");

	pacct_traced_tasks_init();
	ret = pacct_proc_init();
	if (ret) {
		pr_err("could not initialize proc\n");
		goto err;
	}

	ret = pacct_cpu_class_init();
	if (ret) {
		pr_err("could not initialize CPU classes\n");
		goto err;
	}
	ret = pacct_counters_install();
	if (ret) {
		pr_err("could not install counters\n");
		goto err;
	}
	pr_info("installed counters\n");
	ret = pacct_tracepoints_register();
	if (ret) {
		pr_err("could not register tracepoints\n");
		goto err_counters;
	}
	pr_info("registered tracepoints\n");

	// Initialize the powercap interfaces and get the initial CPU frequency caps
	// TODO ret = powercap_init_caps();
	// if (ret) {
	// 	pr_err("powercap init failed: %d\n", ret);
	// 	goto err_counters;
	// }

	// Start the energy estimator work
	pacct_queue_energy_estimator_start();

	// Schedule a delayed work to scan existing tasks and create traced_task entries for them
	// queue_pacct_scan_tasks();

	return 0;

	// Clean up any traced tasks that might have been created before the failure
	// clean_traced_task();

err_tracepoints:
	pacct_tracepoints_unregister();
err_counters:
	pacct_counters_uninstall();
err:
	return ret;
}

static void __exit pacct_energy_exit(void)
{
	pacct_queue_energy_estimator_stop();

	pacct_traced_tasks_clean();
	// Stop the energy estimator work by first
	// pacct_stop_energy_estimator();

	// Clean up for powercap policies and interfaces
	// powercap_cleanup_caps();

	pacct_tracepoints_unregister();
	pacct_counters_uninstall();
	pacct_proc_remove();

	pacct_error_report();

	pr_info("pacct_energy removed\n");
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);
