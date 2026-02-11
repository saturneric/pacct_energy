#!/bin/bash

set -e

# Build the kernel module
make

# Sync the filesystem to ensure that once the module causes a crash, the work is not lost
sync

# Insert the kernel module
sudo modprobe ./pacct_energy.ko

# Run a CPU stress test to generate some context switches and events
sudo taskset -c 4-7 stress-ng --cpu 4 --timeout 3s

# Remove the kernel module
sudo rmmod pacct_energy

# Display the contents of the log file
sudo dmesg | tail -n 256