# pacct_energy: Process ACCT (Accounting) + Energy

This project implements a Linux kernel module called `pacct_energy` that extends
the process accounting subsystem to include energy estimation based on hardware
performance counters. The module tracks various performance events for each
process and estimates the energy consumption of processes when they exit.

What's done in this project:
1. Register trace hooks for process fork and exit events to track the lifecycle
   of processes.
2. Currently, we trace all the processes in the system. 
3. Setup performance counters to monitor specific perf events of the traded
   processes later in a work queue. The events being monitored can be found in
   the header file `pacct_energy.h`.
4. When the context switch out, it will read the performance counter values,
   calculate the diff and store them in the `traced_task` structure.
5. Calculate the energy estimation for each traced process in background, based
   on the counter values and predefined coefficients. It's now done in a period
   of 1 ms.
6. Print the energy estimation for each process when it exits, along with the
   event counts. The related `traced_task` structure will be removed and freed
   carefully after the process exits.

## Context

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
contains the output from the kernel module.

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