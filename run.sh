#!/bin/bash

set -e

# Build the kernel module
make

# Sync the filesystem to ensure that once the module causes a crash, the work is not lost
sync

# Insert the kernel module
sudo modprobe ./pacct_energy.ko enable_power_cap=1 target_mW=25000 

# Run a CPU stress test to generate some context switches and events
sudo turbostat --Summary --show Avg_MHz,Busy%,PkgWatt --interval 1 --quiet -- taskset -c 0-11 stress-ng --cpu 80 --timeout 15s
sleep 5 # For comparison

# Remove the kernel module
sudo rmmod pacct_energy

# Display the contents of the log file
sudo dmesg | tail -n 256