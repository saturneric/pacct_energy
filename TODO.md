# Done

- Estimate power based on wall clock time delta instead of CPU time delta, which
  is a real power estimation under a heavy task context switching scenario, and
  can better reflect the real power of the task.
- Estimate power based on real execution time from scheduler, which can get more
  accurate power of the task by avoiding the noise from sleeping time.
- Read RAPL values directly from the kernel to get more accurate power
  estimation, and conpaire the estimated power with the RAPL power to verify the
  accuracy of the estimation.
- Reduce the cpu frequency when the estimated power is above a certain
  threshold, to save power and energy.


# TODO

- Support Intel Thread Director (ITD) by modifying kernel. 
- Adjust the power estimation model to reduce the value error from the RAPL
  values.
- Userspace tool to read the power and energy values of the processes and show
  in a process tree.
- Current energy estimating model doesn't support E-cores, which can lead to
  inaccurate power estimation when the workload is running on E-cores. We can
  consider adding a separate power estimation model for E-cores to improve the
  accuracy of power estimation.