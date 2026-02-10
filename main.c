#define pr_fmt(fmt) "%s:%s():%d: " fmt, KBUILD_MODNAME, __func__, __LINE__

#include <linux/init.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/printk.h>

#include <asm/msr.h>

MODULE_AUTHOR("pm3");
MODULE_DESCRIPTION("Energy Management Module");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

#define MSR_01D3 0x01D3
#define MSR_0211 0x0211

static struct perf_event* events[4];  // Monitoring up to 4 events

struct task_struct* get_task_by_pid(pid_t pid) {
  struct task_struct* task = NULL;

  rcu_read_lock();
  task = pid_task(find_vpid(pid), PIDTYPE_PID);
  if (task)
    get_task_struct(task);
  rcu_read_unlock();

  return task;  // muss später put_task_struct()
}

static u64 read_event_count(struct perf_event* ev) {
  u64 enabled = 0, running = 0;

  if (!ev)
    return 0;

  return perf_event_read_value(ev, &enabled, &running);
}

static int setup_task_counter(pid_t pid) {
  struct perf_event_attr attr;
  struct task_struct* t;

  memset(&attr, 0, sizeof(attr));
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof(attr);
  attr.config = PERF_COUNT_HW_INSTRUCTIONS;

  attr.disabled = 1;
  attr.exclude_kernel = 0;
  attr.exclude_user = 0;
  attr.exclude_hv = 0;

  t = get_task_by_pid(pid);

  if (!t)
    return -ESRCH;

  events[0] = perf_event_create_kernel_counter(&attr, -1, t, NULL, NULL);
  put_task_struct(t);

  if (IS_ERR(events[0])) {
    long err = PTR_ERR(events[0]);
    return (int)err;
  }

  perf_event_enable(events[0]);
  return 0;
}

static int __init pacct_energy_init(void) {
  pr_info("pacct_energy init\n");
  // Create counter for init
  setup_task_counter(1);
  return 0; /* success */
}

static void __exit pacct_energy_exit(void) {
  pr_info("pacct_energy removed\n");
  if (!IS_ERR(events[0])) {
    u64 count = read_event_count(events[0]);
    pr_info("Counter: %llu\n", count);
    perf_event_disable(events[0]);
    perf_event_release_kernel(events[0]);
  }
  
}

module_init(pacct_energy_init);
module_exit(pacct_energy_exit);