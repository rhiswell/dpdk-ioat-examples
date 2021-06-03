// \ref https://doc.dpdk.org/guides-20.11/rawdevs/ioat.html
// \ref
// https://software.intel.com/content/www/us/en/develop/articles/memory-in-dpdk-part-2-deep-dive-into-iova.html

#include <assert.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rte_dev.h"
#include "rte_ethdev.h"  // Not include this header will cause BUGs
#include "rte_ioat_rawdev.h"
#include "rte_malloc.h"
#include "rte_rawdev.h"

#define KB(x) ((x) << 10)
#define MB(x) ((x) << 20)
#define GB(x) ((x) << 30)

int main(int argc, char *argv[]) {
    int ret;

    // Init the EAL
    ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    argc -= ret;
    argv += ret;

    // Count all the raw devices
    int num_rawdev = rte_rawdev_count();
    printf("Found %d raw devices\n", num_rawdev);

    // Filter out the first functional IOAT device
    struct rte_device *dev_ctx = NULL;
    int dev_id = 0;
    while (dev_id < num_rawdev) {
        struct rte_rawdev_info dev_info = {.dev_private = NULL};
        if (rte_rawdev_info_get(dev_id, &dev_info, 0) == 0 &&
            strcmp(dev_info.driver_name, IOAT_PMD_RAWDEV_NAME_STR) == 0 &&
            rte_rawdev_selftest(dev_id) == 0) {
            printf(
                "First functional IOAT device found: ioat_dev_name = %s, "
                "numa_node = %d\n",
                dev_info.device->name, dev_info.device->numa_node);
            dev_ctx = dev_info.device;
            break;
        }
        dev_id++;
    }
    if (!dev_ctx) {
        printf("IOAT device not found!\n");
        assert(dev_ctx != NULL);
    }

    // Configure the IOAT device
    struct rte_ioat_rawdev_config ioat_dev_conf = {
        // Noted that the ring size must be a power of two, between 64 and 4096.
        .ring_size = 512,      // size of job submission descriptor ring
        .hdls_disable = false  // if set, ignore user-supplied handle params
    };
    struct rte_rawdev_info dev_info = {.dev_private = &ioat_dev_conf};
    ret = rte_rawdev_configure(dev_id, &dev_info, sizeof ioat_dev_conf);
    assert(ret == 0);
    printf("The IOAT device configured: ring_size = %d\n",
           ioat_dev_conf.ring_size);

    ret = rte_rawdev_start(dev_id);
    assert(ret == 0);

    char *gold_msg = "Hello IOAT!";
    size_t buf_size = KB(4);
    assert(buf_size >= sizeof gold_msg);

    // Prepare buffers
    char *src = memalign(KB(4), buf_size), *dst = memalign(KB(4), buf_size);
    assert(src != NULL && dst != NULL);

    // Register memories into DPDK otherwise DPDK rejects for dma mapping. The
    // question is why should we first register these memories into DPDK?
    ret = rte_extmem_register(src, buf_size, NULL, 1, KB(4));
    assert(ret == 0);
    // This will request the kernel module vfio_pci to register memories to
    // IOMMU. For each request, vfio_pci will first pin all the memories, the
    // get the mappings from the page table, and finally write them into the
    // IOMMU. This process can be traced with perf, ie perf trace --no-syscall
    // -e iommu:* -- progname args
    ret = rte_dev_dma_map(dev_ctx, src, (uint64_t)src, buf_size);
    assert(ret == 0);

    ret = rte_extmem_register(dst, buf_size, NULL, 1, KB(4));
    assert(ret == 0);
    ret = rte_dev_dma_map(dev_ctx, dst, (uint64_t)dst, buf_size);
    assert(ret == 0);

    strcpy(src, gold_msg);

    // Submit a data copy request
    ret = rte_ioat_enqueue_copy(dev_id, (uintptr_t)src, (uintptr_t)dst,
                                buf_size, (uintptr_t)src, (uintptr_t)dst);
    assert(ret == 1);
    printf("Copy request submitted\n");

    // Kick the doorbell
    rte_ioat_perform_ops(dev_id);
    printf("Doorbell kicked\n");

    // Poll for the completion
    int ne = 0, total_ops = 1;
    char *src_handle[1], *dst_handle[1];
    printf("Polling for the completion\n");
    do {
        ret = rte_ioat_completed_ops(dev_id, 1, (void *)&src_handle[0],
                                     (void *)&dst_handle[0]);
        if (ret < 0) {
            printf("Poll for completion failed: %s\n", rte_strerror(rte_errno));
            assert(ret >= 0);
        }
        ne += ret;
    } while (ne < total_ops);
    assert(src_handle[0] == src && dst_handle[0] == dst);

    // Check the result
    if (strcmp(dst, gold_msg) == 0) {
        printf("It works!\n");
    } else {
        printf("Copy failed!\n");
    }

    // Shutdown the device
    rte_rawdev_stop(dev_id);

    return 0;
}