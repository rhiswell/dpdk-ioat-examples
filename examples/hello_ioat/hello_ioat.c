// \ref https://doc.dpdk.org/guides-20.11/rawdevs/ioat.html

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rte_ethdev.h"  // Not include this header will cause BUGs
#include "rte_ioat_rawdev.h"
#include "rte_rawdev.h"

#define KB(x) ((x) << 10)
#define MB(x) ((x) << 20)
#define GB(x) ((x) << 30)

int main(int argc, char* argv[]) {
    int ret;

    /* Init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    argc -= ret;
    argv += ret;

    // Count all the raw devices
    int num_rawdev = rte_rawdev_count();
    printf("Found %d raw devices\n", num_rawdev);

    // Filter out the first found IOAT device
    int dev_id, found = 0;
    for (dev_id = 0; dev_id < num_rawdev; dev_id++) {
        struct rte_rawdev_info dev_info = {.dev_private = NULL};
        found = (rte_rawdev_info_get(dev_id, &dev_info, 0) == 0 &&
                 strcmp(dev_info.driver_name, IOAT_PMD_RAWDEV_NAME_STR) == 0);
        if (found) {
            printf(
                "First IOAT device found: ioat_dev_name = %s, numa_node = %d\n",
                dev_info.device->name, dev_info.device->numa_node);
            break;
        }
    }
    if (!found) {
        printf("IOAT device not found!\n");
        assert(found);
    }

    // Configure the IOAT device
    struct rte_ioat_rawdev_config ioat_dev_conf = {
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

    // Prepare buffers
    size_t msg_size = KB(4);
    char* src = malloc(msg_size);
    assert(src != NULL);
    memset(src, 0, msg_size);

    char* gold_msg = "This is a gold!";
    strcpy(src, gold_msg);

    char* dst = malloc(msg_size);
    assert(dst != NULL);
    memset(dst, 0, msg_size);

    // Submit a data copy request
    ret = rte_ioat_enqueue_copy(dev_id, (uint64_t)src, (uint64_t)dst, msg_size,
                                (uintptr_t)src, (uintptr_t)dst);
    assert(ret == 1);

    // Kick the doorbell
    rte_ioat_perform_ops(dev_id);

    // Poll for the completion
    int ne = 0, total_ops = 1;
    uintptr_t src_handle[1], dst_handle[1];
    do {
        ret = rte_ioat_completed_ops(dev_id, 1, src_handle, dst_handle);
        if (ret < 0) {
            printf("Poll for completion failed: %s\n", rte_strerror(rte_errno));
            assert(ret >= 0);
        }
        ne += ret;
    } while (ne < total_ops);
    assert(src_handle[0] == (uintptr_t)src && dst_handle[0] == (uintptr_t)dst);

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