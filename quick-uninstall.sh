#!/bin/bash

set -e
# Remove the kernel module
sudo rmmod pacct_energy

# Display the contents of the log file
sudo dmesg | tail -n 256
