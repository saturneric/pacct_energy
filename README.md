# pacct_energy: Process ACCT (Accounting) + Energy

This project implements a Linux kernel module called `pacct_energy` that extends
the process accounting subsystem to include energy estimation based on hardware
performance counters. The module tracks various performance events for each
process and estimates the energy consumption of processes when they exit.

What's done in this project:
1. Register trace hooks for process fork and exit events to track the lifecycle
   of processes and allocate `traced_task` structs.
2. Currently, we trace all the processes (except kernel threads) in the system. 
3. Setup performance counters via perf to monitor events of the traced
   processes later in a work queue. The events being monitored can be found in
   the header file `pacct_energy.h`.
4. In the work queue two things are done periodically:
   1. Get the performance counters for perf and estimate the energy using a linear model. Calculate the power of the process via the estimated energy and time passed.
   2. Aggregate the stats (energy, power, counters) for all processes and store them together with analog measurements using rapl as global stats
5. Expose the stats of each process and the global stats via the proc filesystem
6. TODO: Explain powercapping

## Context
Is this deprecated? We should no longer require the modified kernel. (TODO: Ensure we no longer use the modified kernel)

This module should use a modified kernel version that export the symbol
`pref_event_read_local()` to read performance counter values in the kernel
atomically. This is necessary to ensure that the reading of performance counters
can be done just after the process is scheduled out, which allows for more
accurate energy estimation. 

If we simply use `pref_event_read_value()` instead, there may be a delay between
the time we read the counters, because it may sleep during the read operation.
Moreover, it may cause bugs as we need to read the pref counters directly in the
trace hooks function, and in this case, unfortunately, sleep is not allowed.

Some works like energy estimation is done in a work queue context. That's
because we cannot do heavy work in the trace hooks, which may cause long latency
and system crash. And function like `perf_event_create_kernel_counter()` is
also not allowed in the trace hooks, because it may sleep. It would not allow
sleeping in the trace hooks, because it may also cause deadlock and system
crash.

Holding a spinlock for a long time is also not allowed in all the situations.
And use a function like `schedule()` which will cause the current executing
context to sleep when holding a spinlock is also not allowed in all the
situations. So we need to be very careful when we write the code for the kernel
module, especially in the trace hooks or any other non-process (e.g. interrupt,
softirq) context.

Currently, the kernel version on this machine is `6.18.4+`, and the modified
kernel can be found at `/data/linux`.

## Definition of Energy Estimation

You can find the detailed definition of energy estimation (e.g., pref counter,
coefficients) in the header file `pacct_energy.h`. 

## Build 

To build the kernel module, simply run `make` in the project directory. This
will compile the `pacct_energy.ko` kernel module.

## Run

Just run `./run.sh` to test the kernel module. It will load the module, run a
CPU stress test to generate some events, and then remove the module. In the end,
it will also print the tail of the kernel log (`dmesg | tail -256`), which
contains debug output from the kernel module.

## Output
The following values are exposed in `/proc/pacct_energy`:

### Global Statistics (`/proc/pacct_energy/global_stats/`)
- `snapshot` - JSON formatted snapshot of all global statistics
- `energy_uj` - Total estimated energy consumption across all processes (microjoules)
- `energy_rapl_uj` - Total energy measured via RAPL (Running Average Power Limit) interface (microjoules)
- `power_mW` - Current estimated power consumption across all processes (milliwatts)
- `power_rapl_mW` - Current power measured via RAPL (milliwatts)
- `rxxyy` -  The  total value, summed over all processes, of the performance counter with umask xx and event code yy

### Per-Process Statistics (`/proc/pacct_energy/<PID>/`)
Each running process has a directory (named by its PID) containing:
- `energy_uj` - Estimated energy consumption of this process (microjoules)
- `power_mW` - Current estimated power consumption of this process (milliwatts)
- `rxxyy` - Individual performance counter values with umask xx and event code yy

All counter values represent cumulative counts since the module was loaded or the process started.

## Caution

You need to install the `clang-format` extension in your vscode editor to format
the code or just run `clang-format` as a command. The code style is based on the
Linux kernel coding style or maybe GNU or Google one, not sure. By the way, you
can find the details in the file `.clang-format`.

When you modify the kernel module in a wrong way, it may crash the system and
you cannot recover from it without external intervention. To recover from the
crash, you need to login to the management system of the lab. Then **reset the
Intel AMT**but not the system itself. After the reset, the system will be back
to normal within 15 seconds.