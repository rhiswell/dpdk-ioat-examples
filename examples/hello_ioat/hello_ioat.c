// \ref https://doc.dpdk.org/guides-20.11/rawdevs/ioat.html
// \ref
// https://software.intel.com/content/www/us/en/develop/articles/memory-in-dpdk-part-2-deep-dive-into-iova.html
// \ref https://www.kernel.org/doc/html/latest/driver-api/vfio.html

#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rte_dev.h"
#include "rte_ethdev.h"  // Not include this header will cause BUGs
#include "rte_ioat_rawdev.h"
#include "rte_malloc.h"
#include "rte_rawdev.h"

#define KB(x) ((x) << 10)
#define MB(x) ((x) << 20)
#define GB(x) ((x) << 30)

int ioat_dma_map(struct rte_device *ioat_dev, const void *addr, size_t len);
int ioat_dma_unmap(struct rte_device *ioat_dev, const void *addr, size_t len);

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
    struct rte_device *ioat_dev = NULL;
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
            ioat_dev = dev_info.device;
            break;
        }
        dev_id++;
    }
    if (!ioat_dev) {
        printf("IOAT device not found!\n");
        assert(ioat_dev != NULL);
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

    // Prepare buffers, including register them to DPDK and IOMMU
    size_t buf_size = KB(8);
    // calloc <=> malloc & memset
    uint8_t *src = calloc(1, buf_size), *padding = calloc(1, buf_size),
            *dst = calloc(1, buf_size);
    assert(src != NULL && padding != NULL && dst != NULL);

    ret = ioat_dma_map(ioat_dev, src, buf_size);
    assert(ret == 0);
    ret = ioat_dma_map(ioat_dev, dst, buf_size);
    assert(ret == 0);

    for (size_t i = 0; i < buf_size; i++) {
        src[i] = rand() % 255;
    }

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
    uint8_t *src_handle[1], *dst_handle[1];
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
    int error = 0;
    for (size_t i = 0; i < buf_size && !error; i++) {
        error = (src[i] != dst[i]);
    }
    (error) ? printf("Copy failed!\n") : printf("It works!\n");

    ret = ioat_dma_unmap(ioat_dev, src, buf_size);
    assert(ret == 0);
    ret = ioat_dma_unmap(ioat_dev, dst, buf_size);
    assert(ret == 0);

    // Shutdown the device
    rte_rawdev_stop(dev_id);

    return 0;
}

int ioat_dma_map(struct rte_device *ioat_dev, const void *addr, size_t len) {
    int ret;

    size_t page_size = getpagesize(), page_shift = log2(page_size),
           page_mask = (1UL << page_shift) - 1;
    printf("page_size = %lu, page_shift = %lu, page_mask = 0x%lx\n", page_size,
           page_shift, page_mask);
    assert(page_size == KB(4) && page_shift == 12);

    size_t num_pages = (((uintptr_t)addr + len - 1) >> page_shift) -
                       ((uintptr_t)addr >> page_shift) + 1;

    uintptr_t aligned_addr = ((uintptr_t)addr) & ~page_mask;

    printf("addr = %p, aligned_addr = %p, num_pages = %lu\n", addr,
           (void *)aligned_addr, num_pages);

    struct rte_memseg_list *msl =
        rte_mem_virt2memseg_list((void *)aligned_addr);
    if (msl != NULL) {
        if (msl->base_va == (void *)aligned_addr &&
            msl->len == num_pages * page_size) {
            printf("Memory was registered\n");
            return 0;
        }
        printf("Memory is overlapped with previously registered memory!\n");
        assert(false);
    }

    // Register memories into DPDK otherwise DPDK rejects for dma
    // mapping. The question is why should we first register these
    // memories into DPDK?
    ret = rte_extmem_register((void *)aligned_addr, page_size * num_pages, NULL,
                              num_pages, page_size);
    if (ret < 0) {
        printf("Failed to register memory to DPDK: %s\n",
               rte_strerror(rte_errno));
        return ret;
    }

    // This will request the kernel module vfio_pci to register memories to
    // IOMMU. For each request, vfio_pci will first pin all the memories, the
    // get the mappings from the page table, and finally write them into the
    // IOMMU. This process can be traced with perf, ie perf trace --no-syscall
    // -e iommu:* -- progname args
    ret = rte_dev_dma_map(ioat_dev, (void *)aligned_addr, aligned_addr,
                          page_size * num_pages);
    if (ret < 0) {
        printf("Failed to register memory to IOMMU: %s\n",
               rte_strerror(rte_errno));
        return -1;
    }

    return 0;
}

int ioat_dma_unmap(struct rte_device *ioat_dev, const void *addr, size_t len) {
    int ret;

    size_t page_size = getpagesize(), page_shift = log2(page_size),
           page_mask = (1UL << page_shift) - 1;
    printf("page_size = %lu, page_shift = %lu, page_mask = 0x%lx\n", page_size,
           page_shift, page_mask);
    assert(page_size == KB(4) && page_shift == 12);

    size_t num_pages = (((uintptr_t)addr + len - 1) >> page_shift) -
                       ((uintptr_t)addr >> page_shift) + 1;

    uintptr_t aligned_addr = ((uintptr_t)addr) & ~page_mask;

    printf("addr = %p, aligned_addr = %p, num_pages = %lu\n", addr,
           (void *)aligned_addr, num_pages);

    struct rte_memseg_list *msl =
        rte_mem_virt2memseg_list((void *)aligned_addr);
    if (msl == NULL) {
        printf("Memory was not registered\n");
        return 0;
    }

    // Dereg the memories from IOMMU
    ret = rte_dev_dma_unmap(ioat_dev, (void *)aligned_addr, aligned_addr,
                            page_size * num_pages);
    if (ret < 0) {
        printf("Failed to deregister memory from IOMMU: %s\n",
               rte_strerror(rte_errno));
        return -1;
    }

    // Dereg the memories from DPDK
    ret = rte_extmem_unregister((void *)aligned_addr, page_size * num_pages);
    if (ret < 0) {
        printf("Failed to deregister memory from DPDK: %s\n",
               rte_strerror(rte_errno));
        return ret;
    }

    return 0;
}