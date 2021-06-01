#!/bin/bash

DPDK_DIR=dpdk-stable-20.11.1
test -d ${DPDK_DIR} || tar -xf dpdk-20.11.1.tar.xz

pushd ${DPDK_DIR}
meson build
pushd build
ninja
sudo ninja install
sudo ldconfig
popd
popd

# Use pkg-config to acquire the DPDK's flags while compiling your applications
pkg-config --cflags libdpdk
pkg-config --libs libdpdk
