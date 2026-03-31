#!/bin/bash

set -e

# Build the kernel module
make

# Sync the filesystem to ensure that once the module causes a crash, the work is not lost
sync

# Insert the kernel module
sudo modprobe ./pacct_energy.ko enable_power_cap=1 target_mW=25000
