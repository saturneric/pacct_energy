#!/bin/bash

set -e

# Build the kernel module
make

# Insert the kernel module
sudo modprobe ./pacct_energy.ko

# Run a CPU stress test to generate some context switches and events
sudo taskset -c 0-12 stress-ng --cpu 12 --timeout 3s

# Remove the kernel module
sudo rmmod pacct_energy