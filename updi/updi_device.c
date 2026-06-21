/**
 * @file updi_device.c
 * @brief Device table lookup. The table itself is generated (updi_device_table.h).
 */
#include "updi_device.h"

#include "updi_device_table.h"

#define UPDI_DEVICE_COUNT (sizeof(updi_device_table) / sizeof(updi_device_table[0]))

const UpdiDevice* updi_device_find_by_id(uint32_t device_id) {
    for(size_t i = 0; i < UPDI_DEVICE_COUNT; i++) {
        if(updi_device_table[i].device_id == device_id) return &updi_device_table[i];
    }
    return NULL;
}

const UpdiDevice* updi_device_table_get(size_t* count) {
    if(count) *count = UPDI_DEVICE_COUNT;
    return updi_device_table;
}
