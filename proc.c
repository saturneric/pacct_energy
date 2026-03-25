#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/debugfs.h>

#include "pacct.h"
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

static void setup_global_proc_file(void)
{
	struct proc_dir_entry *total_stats_dir =
		proc_mkdir(PACCT_GLOBAL_STATS_DIR, pacct_proc_dir);
	if (!total_stats_dir) {
		pr_info("Failed to create " PACCT_GLOBAL_STATS_DIR
			" proc directory");
		return;
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

void setup_proc_file(struct traced_task *entry)
{
	//Create Directory for process
	// we know that 32 byte is enough space for any integer and any counter name or a PID
	char name_buffer[32];
	sprintf(name_buffer, "%d", entry->pid);
	entry->process_dir = proc_mkdir(name_buffer, pacct_proc_dir);

	// expose all the values we have
	proc_create_data("energy_uj", ACCESS_RIGHTS, entry->process_dir, &ops,
			 &entry->energy_uj);
	proc_create_data("power_wallclock_mW", ACCESS_RIGHTS,
			 entry->process_dir, &ops, &entry->power_wallclock_mW);
	proc_create_data("power_cpu_mW", ACCESS_RIGHTS, entry->process_dir,
			 &ops, &entry->power_cpu_mW);
}

void freeProcFile(struct traced_task *entry)
{
	proc_remove(entry->process_dir);
}

void init_proc()
{
	pacct_proc_dir = proc_mkdir(PACCT_PROC_DIR, NULL);
	if (!pacct_proc_dir) {
		pr_info("Failed to create /proc/%s", PACCT_PROC_DIR);
	} else {
		pr_info("pacct_energy: /proc/%s created\n", PACCT_PROC_DIR);
	}
	setup_global_proc_file();
}

void remove_proc()
{
	if (pacct_proc_dir) {
		proc_remove(pacct_proc_dir);
	}
}
