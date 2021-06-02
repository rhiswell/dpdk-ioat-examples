// \ref https://doc.dpdk.org/guides-20.11/rawdevs/ioat.html
// \ref
// https://software.intel.com/content/www/us/en/develop/articles/memory-in-dpdk-part-2-deep-dive-into-iova.html

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rte_ethdev.h"  // Not include this header will cause BUGs
#include "rte_ioat_rawdev.h"
#include "rte_malloc.h"
#include "rte_rawdev.h"

int main(int argc, char* argv[]) {
    int ret;

    // Init the EAL
    ret = rte_eal_init(argc, argv);
    if (ret < 0) rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    argc -= ret;
    argv += ret;

    // Count all the raw devices
    int num_rawdev = rte_rawdev_count();
    printf("Found %d raw devices\n", num_rawdev);

    // Test all the found IOAT devices
    int dev_id;
    for (dev_id = 0; dev_id < num_rawdev; dev_id++) {
        struct rte_rawdev_info dev_info = {.dev_private = NULL};
        if (rte_rawdev_info_get(dev_id, &dev_info, 0) == 0 &&
            strcmp(dev_info.driver_name, IOAT_PMD_RAWDEV_NAME_STR) == 0) {
        }
        printf("IOAT device found: ioat_dev_name = %s, numa_node = %d\n",
               dev_info.device->name, dev_info.device->numa_node);

        if (rte_rawdev_selftest(dev_id) == 0) {
            printf("Self test passed!\n");
        } else {
            printf("Self test failed!\n");
        }
    }

    return 0;
}