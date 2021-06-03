#!/bin/bash

bdf=${1:-}

[[ $bdf == "" ]] && {
    echo "Usage: $0 <device_bdf>"
    exit -1
}

sudo readlink -f /sys/bus/pci/devices/${bdf}/iommu_group
