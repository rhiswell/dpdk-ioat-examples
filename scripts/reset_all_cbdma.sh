#!/bin/bash

set -x

# Load required drivers is the first step to use devices, eg CBDMA, in userspace
# Noted that vfio-pci requires the iommu is enabled in OS, ie intel_iommu=on.
sudo modprobe vfio_pci

# Bind a CBDMA device with the vfio-pci driver. All the CBDMAs can also be
# queried with the dpdk-devbind tool.
ioat_dev_list=$(dpdk-devbind.py --status-dev misc | grep "Crystal Beach DMA" | awk '{print $1}')
for ioat_dev in ${ioat_dev_list}; do
    sudo dpdk-devbind.py -b ioatdma $ioat_dev
    sudo dpdk-devbind.py -b vfio-pci $ioat_dev
done

dpdk-devbind.py --status-dev misc
