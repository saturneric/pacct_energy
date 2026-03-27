#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/debugfs.h>

#include "proc.h"

#define PACCT_PROC_DIR "pacct_energy"
#define PACCT_GLOBAL_STATS_DIR "global_stats"
#define ACCESS_RIGHTS 0444

struct proc_dir_entry *pacct_proc_dir;

static int pacct_int_show(struct seq_file *m, void *v)
{
	s64 *value = m->private;
	seq_printf(m, "%lld\n", *value);
	return 0;
}

static int pacct_int_open(struct inode *inode, struct file *file)
{
	return single_open(file, pacct_int_show, pde_data(inode));
}

static const struct proc_ops ops = {
	.proc_open = pacct_int_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int setup_global_proc_file(void)
{
	struct proc_dir_entry *total_stats_dir =
		proc_mkdir(PACCT_GLOBAL_STATS_DIR, pacct_proc_dir);
	if (!total_stats_dir) {
		pr_info("Failed to create " PACCT_GLOBAL_STATS_DIR
			" proc directory");
		return -1;
	}
	proc_create_data("energy_uj", ACCESS_RIGHTS, total_stats_dir, &ops,
			 &global_stats.energy);
	proc_create_data("power_mW", ACCESS_RIGHTS, total_stats_dir, &ops,
			 &global_stats.power);
	proc_create_data("energy_rapl_uj", ACCESS_RIGHTS, total_stats_dir, &ops,
			 &global_stats.energy_rapl);
	proc_create_data("power_rapl_mW", ACCESS_RIGHTS, total_stats_dir, &ops,
			 &global_stats.power_rapl);
}

int pacct_proc_file_setup(struct traced_task *entry)
{
	if (entry->process_dir != NULL || pacct_proc_dir == NULL) {
		return -1;
	}

	// we know that 32 byte is enough space for anything we do
	char name_buffer[32];
	sprintf(name_buffer, "%d", entry->pid);
	entry->process_dir = proc_mkdir(name_buffer, pacct_proc_dir);
	if (entry->process_dir == NULL) {
		pr_err("error in proc_mkdir\n");
		return -1;
	}

	// expose all the values we have
	proc_create_data("energy_uj", ACCESS_RIGHTS, entry->process_dir, &ops,
			 &entry->energy_uj);
	proc_create_data("power_wallclock_mW", ACCESS_RIGHTS,
			 entry->process_dir, &ops, &entry->power_wallclock_mW);
	proc_create_data("power_cpu_mW", ACCESS_RIGHTS, entry->process_dir,
			 &ops, &entry->power_cpu_mW);
	return 0;
}

void pacct_proc_file_free(struct traced_task *entry)
{
	if (entry->process_dir != NULL) {
		proc_remove(entry->process_dir);
		entry->process_dir = NULL;
	}
}

int pacct_proc_init(void)
{
	pacct_proc_dir = proc_mkdir(PACCT_PROC_DIR, NULL);
	if (!pacct_proc_dir) {
		pr_info("Failed to create /proc/%s", PACCT_PROC_DIR);
		return -1;
	} else {
		pr_info("pacct_energy: /proc/%s created\n", PACCT_PROC_DIR);
	}
	if (setup_global_proc_file()) {
		return -1;
	}
	return 0;
}

void pacct_proc_remove(void)
{
	if (pacct_proc_dir) {
		proc_remove(pacct_proc_dir);
	}
}
