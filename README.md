# Examples of Using I/OAT in Userspace

## Quick start

```bash
# Install DPDK
cd drivers && bash build_and_install_dpdk.sh
# Bind I/OAT devices to the vfio-pci driver and reserve some hugepages
bash scripts/reset_all_cbdma.sh
bash scripts/set_hugepages.sh
# Build any example and run it (with the DPDK log is disabled)
cd examples/hello_ioat && make
sudo ./build/hello_ioat --iova-mode=va --log-level=0
```
