#!/bin/bash

set -x

# Load required drivers is the first step to use devices, eg CBDMA, in userspace
# Noted that vfio-pci requires the iommu is enabled in OS, ie intel_iommu=on.
sudo modprobe vfio-pci

# Bind a CBDMA device with the vfio-pci driver. All the CBDMAs can also be
# queried with the dpdk-devbind tool.
sudo dpdk-devbind.py -b vfio-pci 0000:00:04.0
