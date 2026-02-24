#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/debugfs.h>

#include "pacct.h"
#include "proc.h"

#define PACCT_PROC_DIR "pacct_energy"
struct proc_dir_entry *pacct_proc_dir;

void init_proc() {
	pacct_proc_dir = proc_mkdir(PACCT_PROC_DIR, NULL);
	if (!pacct_proc_dir) {
		pr_info("Failed to create /proc/%s", PACCT_PROC_DIR);
	}
	pr_info("pacct_energy: /proc/%s created\n", PACCT_PROC_DIR);
}

void remove_proc() {
	if (pacct_proc_dir) {
		proc_remove(pacct_proc_dir);
	}
}

static int pacct_int_show(struct seq_file *m, void *v)
{
	atomic64_t *value = m->private;
	seq_printf(m, "%lld\n", atomic64_read(value));
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


void setUpProcFile(struct traced_task *entry) {
    //Create Directory for process
	char *strPid = kasprintf(GFP_ATOMIC, "%d", entry->pid);
	entry->proc_entry.process_dir = proc_mkdir(strPid, pacct_proc_dir);
    kfree(strPid);

	proc_create_data("energy_uj", 0444, entry->proc_entry.process_dir, &ops,
			 &entry->energy);
	for (size_t i = 0; i < PACCT_TRACED_EVENT_COUNT; i++)
	{
		char *name = kasprintf(GFP_ATOMIC, "r%u%s", tracked_events[i].umask, tracked_events[i].event_code);
		//proc_create_data(name, 0444, entry->proc_entry.process_dir, &ops, &entry->counts[i]); //TODO: Implement
		kfree(name);
	}
	
}

void freeProcFile(struct traced_task *entry) {
    proc_remove(entry->proc_entry.process_dir);
}



