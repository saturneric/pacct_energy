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

static const struct proc_ops s64_ops = {
	.proc_open = pacct_int_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};


static void create_counter_files(char *name_buffer, struct proc_dir_entry *dir, s64 *counters)
{	
	for (size_t i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		sprintf(name_buffer, "r%02x%02x", tracked_events[i].umask,
			tracked_events[i].event_code);
		proc_create_data(name_buffer, ACCESS_RIGHTS, dir, &s64_ops,
					&counters[i]);
	}
}

// Snapshot file that reads all global_stats atomically
static int pacct_snapshot_show(struct seq_file *m, void *v)
{
	unsigned int seq;
	struct stats snapshot;

	// Read a consistent snapshot of all global_stats fields
	do {
		seq = read_seqbegin(&global_stats_lock);
		snapshot = global_stats;
	} while (read_seqretry(&global_stats_lock, seq));

	// Print in json format
	seq_printf(m, "{\n");
	seq_printf(m, "  \"energy_uj\":      %lld,\n", snapshot.energy);
	seq_printf(m, "  \"energy_rapl_uj\": %lld,\n", snapshot.energy_rapl);
	seq_printf(m, "  \"power_mW\"     : %lld,\n", snapshot.power);
	seq_printf(m, "  \"power_rapl_mW\": %lld,\n", snapshot.power_rapl);
	seq_printf(m, "  \"counters\": {\n");
	for (int i = 0; i < PACCT_TRACED_EVENT_COUNT; i++) {
		seq_printf(m, "    \"r%02x%02x\": %lld%s\n",
			   tracked_events[i].umask,
			   tracked_events[i].event_code,
			   snapshot.counter[i],
			   (i < PACCT_TRACED_EVENT_COUNT - 1) ? "," : "");
	}
	seq_printf(m, "  }\n");
	seq_printf(m, "}\n");
	
	return 0;
}

static int pacct_snapshot_open(struct inode *inode, struct file *file)
{
	return single_open(file, pacct_snapshot_show, NULL);
}

static const struct proc_ops snapshot_ops = {
	.proc_open = pacct_snapshot_open,
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
	proc_create_data("energy_uj", ACCESS_RIGHTS, total_stats_dir, &s64_ops,
			 &global_stats.energy);
	proc_create_data("power_mW", ACCESS_RIGHTS, total_stats_dir, &s64_ops,
			 &global_stats.power);
	char name_buffer[32];
	create_counter_files(name_buffer, total_stats_dir, global_stats.counter);
	proc_create_data("energy_rapl_uj", ACCESS_RIGHTS, total_stats_dir, &s64_ops,
			 &global_stats.energy_rapl);
	proc_create_data("power_rapl_mW", ACCESS_RIGHTS, total_stats_dir, &s64_ops,
			 &global_stats.power_rapl);
	
	// Create snapshot file that provides atomic view of global_stats
	proc_create("snapshot", ACCESS_RIGHTS, total_stats_dir, &snapshot_ops);
}

void setup_proc_file(struct traced_task *entry)
{
	//Create Directory for process
	// we know that 32 byte is enough space for any integer and any counter name or a PID
	char name_buffer[32];
	sprintf(name_buffer, "%d", entry->pid);
	entry->process_dir = proc_mkdir(name_buffer, pacct_proc_dir);

	// expose all the values we have
	proc_create_data("energy_uj", ACCESS_RIGHTS, entry->process_dir, &s64_ops,
				&entry->energy_uj);
	proc_create_data("power_mW", ACCESS_RIGHTS, entry->process_dir, &s64_ops,
				&entry->power_mW);
	create_counter_files(name_buffer, entry->process_dir, entry->counts);
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
