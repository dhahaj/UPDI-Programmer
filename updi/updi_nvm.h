/**
 * @file updi_nvm.h
 * @brief UPDI NVM programming: enter/leave progmode, unlock, chip erase, flash page
 *        erase/write, fuse read/write, and chunked memory read.
 *
 * Variant-pluggable: the NVMCTRL register map and command set differ between
 * NVM P:0 (tinyAVR 0/1/2, megaAVR 0) and NVM P:3 (AVR EA). Both are ported from
 * pymcuprog (nvmp0.py / nvmp3.py). Pure protocol — depends only on updi_link + updi_device.
 */
#pragma once

#include "updi_link.h"
#include "updi_device.h"

typedef struct {
    UpdiLink* link;
    const UpdiDevice* device;
} UpdiNvm;

/** Parsed System Information Block fields used for NVM-variant selection. */
typedef struct {
    char family[12]; /* e.g. "AVR EA", "tinyAVR", "megaAVR" (null-terminated) */
    uint8_t nvm_version; /* 0 = P:0, 3 = P:3, etc. (the digit from "P:x") */
} UpdiSibInfo;

/* The signature row sits at 0x1100 on every supported part, so it can be read before
 * the device geometry is known (used during identification). */
#define UPDI_SIGROW_DEFAULT_ADDRESS 0x1100u

void updi_nvm_init(UpdiNvm* nvm, UpdiLink* link, const UpdiDevice* device);
/** Bind the device geometry once it has been identified from the signature. */
void updi_nvm_set_device(UpdiNvm* nvm, const UpdiDevice* device);

/**
 * Read and parse the SIB to discover the NVM interface version. Independent of address
 * size (the SIB instruction carries no memory address), so callable right after
 * updi_link_activate() and before the address-size is chosen. Mirrors decode_sib().
 */
UpdiStatus updi_nvm_read_sib_info(UpdiLink* link, UpdiSibInfo* info);

/* ---- programming mode ---- */
UpdiStatus updi_nvm_enter_progmode(UpdiNvm* nvm);
UpdiStatus updi_nvm_leave_progmode(UpdiNvm* nvm);
bool updi_nvm_in_progmode(UpdiNvm* nvm);
bool updi_nvm_is_locked(UpdiNvm* nvm);
/** Unlock a locked device via the chip-erase KEY path (erases everything). */
UpdiStatus updi_nvm_unlock(UpdiNvm* nvm);

/* ---- identification ---- */
/** Read the 3 signature bytes from the sigrow. Requires progmode (or unlocked). */
UpdiStatus updi_nvm_read_signature(UpdiNvm* nvm, uint8_t sig[3]);
/** Read the device revision letter source byte (syscfg_base+1). */
UpdiStatus updi_nvm_read_revision(UpdiNvm* nvm, uint8_t* out);

/* ---- erase ---- */
/** Chip erase using the NVM controller (device must be in progmode and unlocked). */
UpdiStatus updi_nvm_chip_erase(UpdiNvm* nvm);
/** Erase a single flash page at @p address (absolute mapped address). */
UpdiStatus updi_nvm_erase_flash_page(UpdiNvm* nvm, uint32_t address);

/* ---- read ---- */
/** Read @p size bytes from @p address (absolute), chunked internally to UPDI limits. */
UpdiStatus updi_nvm_read(UpdiNvm* nvm, uint32_t address, uint8_t* buf, size_t size);

/* ---- write ---- */
/**
 * Program one flash page (or page fragment). @p address must be page-aligned and @p len
 * must not exceed flash_page_size and must be even (word access). Loads the page buffer
 * and commits with the variant's WRITE_PAGE command (assumes the page is already erased,
 * e.g. after a chip erase).
 */
UpdiStatus updi_nvm_write_flash_page(UpdiNvm* nvm, uint32_t address, const uint8_t* data, size_t len);

/* ---- fuses ---- */
/** Read one fuse byte by index (0..fuses_size-1). */
UpdiStatus updi_nvm_read_fuse(UpdiNvm* nvm, uint8_t index, uint8_t* out);
/** Write one fuse byte by index. Path differs per NVM variant (see updi_nvm.c). */
UpdiStatus updi_nvm_write_fuse(UpdiNvm* nvm, uint8_t index, uint8_t value);
