# pacct_energy: Process ACCT (Accounting) + Energy

This project implements a Linux kernel module called `pacct_energy` that extends
the process accounting subsystem to include energy estimation based on hardware
performance counters. The module tracks various performance events for each
process and estimates the energy consumption of processes when they exit.

What's done in this project:
1. Register trace hooks for process fork and exit events to track the lifecycle of processes.
2. Currently, Newly created processes after module load will be traced, but not
   the existing ones when the module is loaded. For each process being traced,
   it will store all related information in a `traced_task` structure.
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
context to sleep is also not allowed in all the situations. So we need to be
very careful when we write the code for the kernel module, especially in the
trace hooks or any other non-process (e.g. interrupt, softirq) context.

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

## Result 

After running the script, you should see output similar to the following:

```
[  759.346589] pacct_energy:pacct_energy_init():179: pacct_energy init
[  759.348461] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5285, COMM run.sh
[  759.350033] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5287, COMM systemd-udevd
[  759.354448] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5288, COMM sudo
[  759.355281] pacct_energy:pacct_process_exit():140: Process exiting: PID 5288, COMM "unix_chkpwd", energy estimate 0 (uJ)
[  759.358683] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5289, COMM sudo
[  759.359052] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5290, COMM sudo
[  759.365570] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5291, COMM stress-ng
[  759.365623] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5292, COMM stress-ng
[  759.365697] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5293, COMM stress-ng
[  759.365776] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5294, COMM stress-ng
[  759.938472] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5295, COMM node
[  759.940123] pacct_energy:pacct_process_exit():140: Process exiting: PID 5295, COMM "which", energy estimate 0 (uJ)
[  759.940649] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5296, COMM node
[  759.948251] pacct_energy:pacct_process_exit():140: Process exiting: PID 5296, COMM "ps", energy estimate 8 (uJ)
[  759.948254] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5296:
[  759.948255] pacct_energy:pacct_process_exit():149: [0]: count=3484835, diff=82376
[  759.948256] pacct_energy:pacct_process_exit():149: [1]: count=4224239, diff=100245
[  759.948257] pacct_energy:pacct_process_exit():149: [2]: count=844763, diff=21788
[  759.948257] pacct_energy:pacct_process_exit():149: [3]: count=877059, diff=21030
[  759.948258] pacct_energy:pacct_process_exit():149: [4]: count=488, diff=11
[  759.948259] pacct_energy:pacct_process_exit():149: [5]: count=4154530, diff=100752
[  759.948259] pacct_energy:pacct_process_exit():149: [6]: count=10869, diff=279
[  759.948260] pacct_energy:pacct_process_exit():149: [7]: count=413105, diff=10219
[  759.948909] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5297, COMM node
[  759.951021] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5298, COMM cpuUsage.sh
[  759.952120] pacct_energy:pacct_process_exit():140: Process exiting: PID 5298, COMM "sed", energy estimate 0 (uJ)
[  759.952525] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5299, COMM cpuUsage.sh
[  759.953207] pacct_energy:pacct_process_exit():140: Process exiting: PID 5299, COMM "cat", energy estimate 0 (uJ)
[  759.953474] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5300, COMM cpuUsage.sh
[  759.954116] pacct_energy:pacct_process_exit():140: Process exiting: PID 5300, COMM "cat", energy estimate 0 (uJ)
[  759.954370] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5301, COMM cpuUsage.sh
[  759.955016] pacct_energy:pacct_process_exit():140: Process exiting: PID 5301, COMM "cat", energy estimate 0 (uJ)
[  759.955235] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5302, COMM cpuUsage.sh
[  759.955856] pacct_energy:pacct_process_exit():140: Process exiting: PID 5302, COMM "cat", energy estimate 0 (uJ)
[  759.956119] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5303, COMM cpuUsage.sh
[  759.956772] pacct_energy:pacct_process_exit():140: Process exiting: PID 5303, COMM "cat", energy estimate 0 (uJ)
[  759.956991] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5304, COMM cpuUsage.sh
[  760.462371] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5305, COMM node
[  760.464936] pacct_energy:pacct_process_exit():140: Process exiting: PID 5305, COMM "git", energy estimate 0 (uJ)
[  760.467465] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5306, COMM node
[  760.470160] pacct_energy:pacct_process_exit():140: Process exiting: PID 5306, COMM "git", energy estimate 0 (uJ)
[  760.472764] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5307, COMM node
[  760.475609] pacct_energy:pacct_process_exit():140: Process exiting: PID 5307, COMM "git", energy estimate 102 (uJ)
[  760.475613] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5307:
[  760.475614] pacct_energy:pacct_process_exit():149: [0]: count=2259454, diff=1865540
[  760.475616] pacct_energy:pacct_process_exit():149: [1]: count=1532502, diff=1271549
[  760.475617] pacct_energy:pacct_process_exit():149: [2]: count=840989, diff=698213
[  760.475618] pacct_energy:pacct_process_exit():149: [3]: count=314426, diff=262252
[  760.475619] pacct_energy:pacct_process_exit():149: [4]: count=856, diff=715
[  760.475620] pacct_energy:pacct_process_exit():149: [5]: count=1515814, diff=1270831
[  760.475621] pacct_energy:pacct_process_exit():149: [6]: count=7458, diff=6263
[  760.475622] pacct_energy:pacct_process_exit():149: [7]: count=265021, diff=223242
[  760.477320] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5308, COMM node
[  760.480071] pacct_energy:pacct_process_exit():140: Process exiting: PID 5308, COMM "git", energy estimate 0 (uJ)
[  760.859333] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5309, COMM node
[  760.861847] pacct_energy:pacct_process_exit():140: Process exiting: PID 5309, COMM "git", energy estimate 0 (uJ)
[  760.957702] pacct_energy:pacct_process_exit():140: Process exiting: PID 5304, COMM "sleep", energy estimate 0 (uJ)
[  760.958014] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5310, COMM cpuUsage.sh
[  760.959196] pacct_energy:pacct_process_exit():140: Process exiting: PID 5310, COMM "sed", energy estimate 0 (uJ)
[  760.959646] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5311, COMM cpuUsage.sh
[  760.960352] pacct_energy:pacct_process_exit():140: Process exiting: PID 5311, COMM "cat", energy estimate 0 (uJ)
[  760.960661] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5312, COMM cpuUsage.sh
[  760.960930] pacct_energy:pacct_process_exit():140: Process exiting: PID 5312, COMM "cpuUsage.sh", energy estimate 0 (uJ)
[  760.961244] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5313, COMM cpuUsage.sh
[  760.962029] pacct_energy:pacct_process_exit():140: Process exiting: PID 5313, COMM "cat", energy estimate 0 (uJ)
[  760.962416] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5314, COMM cpuUsage.sh
[  760.962662] pacct_energy:pacct_process_exit():140: Process exiting: PID 5314, COMM "cpuUsage.sh", energy estimate 0 (uJ)
[  760.962889] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5315, COMM cpuUsage.sh
[  760.963800] pacct_energy:pacct_process_exit():140: Process exiting: PID 5315, COMM "cat", energy estimate 0 (uJ)
[  760.964074] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5316, COMM cpuUsage.sh
[  760.964375] pacct_energy:pacct_process_exit():140: Process exiting: PID 5316, COMM "cpuUsage.sh", energy estimate 0 (uJ)
[  760.964681] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5317, COMM cpuUsage.sh
[  760.965289] pacct_energy:pacct_process_exit():140: Process exiting: PID 5317, COMM "cat", energy estimate 0 (uJ)
[  760.965638] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5318, COMM cpuUsage.sh
[  760.965858] pacct_energy:pacct_process_exit():140: Process exiting: PID 5318, COMM "cpuUsage.sh", energy estimate 0 (uJ)
[  760.966150] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5319, COMM cpuUsage.sh
[  760.966841] pacct_energy:pacct_process_exit():140: Process exiting: PID 5319, COMM "cat", energy estimate 0 (uJ)
[  760.967129] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5320, COMM cpuUsage.sh
[  760.967317] pacct_energy:pacct_process_exit():140: Process exiting: PID 5320, COMM "cpuUsage.sh", energy estimate 0 (uJ)
[  760.967458] pacct_energy:pacct_process_exit():140: Process exiting: PID 5297, COMM "cpuUsage.sh", energy estimate 267 (uJ)
[  760.967462] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5297:
[  760.967463] pacct_energy:pacct_process_exit():149: [0]: count=19940484, diff=38091
[  760.967465] pacct_energy:pacct_process_exit():149: [1]: count=14118292, diff=33572
[  760.967467] pacct_energy:pacct_process_exit():149: [2]: count=5393021, diff=10041
[  760.967468] pacct_energy:pacct_process_exit():149: [3]: count=3062729, diff=6944
[  760.967469] pacct_energy:pacct_process_exit():149: [4]: count=3156, diff=5
[  760.967470] pacct_energy:pacct_process_exit():149: [5]: count=14082718, diff=30407
[  760.967471] pacct_energy:pacct_process_exit():149: [6]: count=53305, diff=107
[  760.967473] pacct_energy:pacct_process_exit():149: [7]: count=1810449, diff=3523
[  762.367074] pacct_energy:pacct_process_exit():140: Process exiting: PID 5293, COMM "stress-ng-cpu", energy estimate 732912 (uJ)
[  762.367074] pacct_energy:pacct_process_exit():140: Process exiting: PID 5291, COMM "stress-ng-cpu", energy estimate 1421295 (uJ)
[  762.367078] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5291:
[  762.367079] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5293:
[  762.367080] pacct_energy:pacct_process_exit():149: [0]: count=9665487463, diff=1338078967
[  762.367080] pacct_energy:pacct_process_exit():149: [0]: count=4983345540, diff=2535253753
[  762.367081] pacct_energy:pacct_process_exit():149: [1]: count=7728692052, diff=3933656586
[  762.367082] pacct_energy:pacct_process_exit():149: [1]: count=14978254918, diff=2074211598
[  762.367083] pacct_energy:pacct_process_exit():149: [2]: count=6542735, diff=570535
[  762.367083] pacct_energy:pacct_process_exit():149: [2]: count=8035477, diff=3194907
[  762.367084] pacct_energy:pacct_process_exit():149: [3]: count=856372933, diff=434430749
[  762.367084] pacct_energy:pacct_process_exit():149: [3]: count=1661577276, diff=231708238
[  762.367085] pacct_energy:pacct_process_exit():149: [4]: count=1208, diff=39
[  762.367085] pacct_energy:pacct_process_exit():149: [4]: count=825, diff=130
[  762.367087] pacct_energy:pacct_process_exit():149: [5]: count=14978217085, diff=2074211590
[  762.367087] pacct_energy:pacct_process_exit():149: [5]: count=7728677723, diff=3933656527
[  762.367088] pacct_energy:pacct_process_exit():149: [6]: count=51826, diff=23171
[  762.367088] pacct_energy:pacct_process_exit():149: [6]: count=56321, diff=6741
[  762.367089] pacct_energy:pacct_process_exit():149: [7]: count=3054041447, diff=418582747
[  762.367089] pacct_energy:pacct_process_exit():149: [7]: count=1573280772, diff=801739900
[  762.368028] pacct_energy:pacct_process_exit():140: Process exiting: PID 5292, COMM "stress-ng-cpu", energy estimate 734607 (uJ)
[  762.368032] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5292:
[  762.368033] pacct_energy:pacct_process_exit():149: [0]: count=4982143480, diff=2534628969
[  762.368034] pacct_energy:pacct_process_exit():149: [1]: count=7749999887, diff=3946046500
[  762.368035] pacct_energy:pacct_process_exit():149: [2]: count=6661730, diff=2669889
[  762.368036] pacct_energy:pacct_process_exit():149: [3]: count=858743515, diff=436648593
[  762.368037] pacct_energy:pacct_process_exit():149: [4]: count=719, diff=133
[  762.368037] pacct_energy:pacct_process_exit():149: [5]: count=7749991915, diff=3946046565
[  762.368038] pacct_energy:pacct_process_exit():149: [6]: count=45937, diff=20494
[  762.368039] pacct_energy:pacct_process_exit():149: [7]: count=1572025181, diff=799141421
[  762.368238] pacct_energy:pacct_process_exit():140: Process exiting: PID 5294, COMM "stress-ng-cpu", energy estimate 1537212 (uJ)
[  762.368241] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5294:
[  762.368241] pacct_energy:pacct_process_exit():149: [0]: count=10427095786, diff=6986908
[  762.368242] pacct_energy:pacct_process_exit():149: [1]: count=16242694234, diff=4416355
[  762.368243] pacct_energy:pacct_process_exit():149: [2]: count=10066005, diff=21628
[  762.368244] pacct_energy:pacct_process_exit():149: [3]: count=1803509329, diff=1010511
[  762.368245] pacct_energy:pacct_process_exit():149: [4]: count=1235, diff=18
[  762.368245] pacct_energy:pacct_process_exit():149: [5]: count=16242675454, diff=4416449
[  762.368246] pacct_energy:pacct_process_exit():149: [6]: count=78432, diff=167
[  762.368247] pacct_energy:pacct_process_exit():149: [7]: count=3299489770, diff=2830809
[  762.368546] pacct_energy:pacct_process_exit():140: Process exiting: PID 5290, COMM "stress-ng", energy estimate 1 (uJ)
[  762.368548] pacct_energy:pacct_process_exit():146: Event counts for exiting PID 5290:
[  762.368549] pacct_energy:pacct_process_exit():149: [0]: count=23617643, diff=93470
[  762.368550] pacct_energy:pacct_process_exit():149: [1]: count=42789397, diff=54279
[  762.368550] pacct_energy:pacct_process_exit():149: [2]: count=4264202, diff=56250
[  762.368551] pacct_energy:pacct_process_exit():149: [3]: count=9084932, diff=11740
[  762.368552] pacct_energy:pacct_process_exit():149: [4]: count=7623, diff=99
[  762.368552] pacct_energy:pacct_process_exit():149: [5]: count=42707834, diff=54519
[  762.368553] pacct_energy:pacct_process_exit():149: [6]: count=21212, diff=516
[  762.368554] pacct_energy:pacct_process_exit():149: [7]: count=3472165, diff=14011
[  762.368804] pacct_energy:pacct_process_exit():140: Process exiting: PID 5289, COMM "sudo", energy estimate 0 (uJ)
[  762.370282] pacct_energy:pacct_process_exit():140: Process exiting: PID 5285, COMM "sudo", energy estimate 0 (uJ)
[  762.370736] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5321, COMM run.sh
[  762.375710] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5322, COMM sudo
[  762.376574] pacct_energy:pacct_process_exit():140: Process exiting: PID 5322, COMM "unix_chkpwd", energy estimate 0 (uJ)
[  762.380420] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5323, COMM sudo
[  762.380775] pacct_energy:pacct_process_fork():116: Start to trace new process: PID 5324, COMM sudo
[  762.382790] pacct_energy:pacct_energy_exit():273: pacct_energy removed
```