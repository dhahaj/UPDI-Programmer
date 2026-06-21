/**
 * @file updi_device.h
 * @brief Target device geometry table (signature -> flash/eeprom/fuse layout + NVM variant).
 *
 * The geometry rows are generated from Microchip's device packs by
 * scripts/gen_device_table.py into updi_device_table.h. Unknown signatures are rejected
 * with a clear error rather than guessing geometry.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** Which NVMCTRL programming variant a part uses (see updi_nvm.c). */
typedef enum {
    UpdiNvmVariantP0 = 0, /* tinyAVR 0/1/2, megaAVR 0 — 16-bit addressing, page-oriented */
    UpdiNvmVariantP3, /* AVR EA — 24-bit addressing, page-oriented */
} UpdiNvmVariant;

typedef struct {
    uint32_t device_id; /* 3-byte signature as 0x1Exxxx (big-endian order) */
    const char* name;

    uint32_t flash_address; /* mapped base of flash in the UPDI data space */
    uint32_t flash_size;
    uint16_t flash_page_size;

    uint32_t eeprom_address;
    uint16_t eeprom_size;
    uint16_t eeprom_page_size;

    uint32_t fuses_address;
    uint16_t fuses_size;

    uint32_t userrow_address;
    uint16_t userrow_size;
    uint16_t userrow_page_size;

    uint32_t sigrow_address; /* signature row (3 sig bytes live here) */
    uint32_t nvmctrl_base;
    uint32_t syscfg_base; /* syscfg_base+1 = device revision */

    uint8_t address_bytes; /* 2 (16-bit DL) or 3 (24-bit DL) */
    uint8_t nvm_variant; /* UpdiNvmVariant */
} UpdiDevice;

/** Look up a device by its 3-byte signature id (0x1Exxxx). Returns NULL if unknown. */
const UpdiDevice* updi_device_find_by_id(uint32_t device_id);

/** Build a device id from the 3 signature bytes as read from the target (big-endian). */
static inline uint32_t updi_device_id_from_sig(const uint8_t sig[3]) {
    return ((uint32_t)sig[0] << 16) | ((uint32_t)sig[1] << 8) | (uint32_t)sig[2];
}

/** Access the raw table (mainly for the UI list / tests). */
const UpdiDevice* updi_device_table_get(size_t* count);
