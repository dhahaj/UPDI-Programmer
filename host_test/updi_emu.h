/**
 * @file updi_emu.h
 * @brief A small UPDI *target* emulator used as a mock transport for host unit tests.
 *
 * It decodes the exact instruction byte stream our updi/ layer emits (SYNCH framing,
 * LDCS/STCS, LDS/STS, ST/LD ptr, REPEAT, KEY, SIB) and produces faithful responses
 * (ACKs, data, SIB, status). It models flash + a small I/O region (sigrow, NVMCTRL,
 * fuses) so a write-page-then-read-back test verifies the whole choreography end to end.
 *
 * This is deliberately NOT a complete silicon model — just enough to validate the port.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../updi/updi_transport.h"

#define EMU_FLASH_SIZE 0x10000u
#define EMU_IO_BASE 0x1000u
#define EMU_IO_SIZE 0x0600u
#define EMU_RX_CAP 2048u
#define EMU_TX_CAP 65536u

typedef struct {
    /* configuration */
    uint32_t flash_base;
    uint32_t nvmctrl_base;
    uint8_t status_offset; /* NVMCTRL status register offset (reads as ready=0) */
    uint8_t ctrla_offset; /* NVMCTRL command register offset */
    uint8_t fuse_data_offset; /* P:0 fuse DATA register offset */
    uint8_t fuse_addr_offset; /* P:0 fuse ADDR register offset */
    uint8_t cmd_write_fuse; /* P:0 WRITE_FUSE command (applies DATA->fuse addr) */
    uint8_t cmd_chip_erase; /* chip-erase command (resets emulated flash to 0xFF) */
    uint32_t sigrow_addr;
    uint8_t sig[3];
    uint8_t address_bytes; /* expected 2 or 3 (for sanity, not strictly required) */
    uint8_t sib[32];
    uint8_t sib_len;

    /* memories */
    uint8_t flash[EMU_FLASH_SIZE];
    uint8_t io[EMU_IO_SIZE];
    uint8_t cs[16];

    /* protocol state */
    int state;
    uint8_t acc[8];
    int acclen;
    int need;
    uint8_t pending_cs_addr;
    uint32_t cur_addr;
    int data_size; /* response/data width in bytes (1 or 2) */
    uint32_t ptr;
    int repeat_next; /* N from REPEAT; next inc op runs N+1 times */
    int inc_left; /* remaining executions/bytes for the active inc op */
    bool nvm_key;
    bool erase_key;

    /* queues / logs */
    uint8_t rx[EMU_RX_CAP];
    size_t rx_head, rx_tail;
    uint8_t tx[EMU_TX_CAP];
    size_t tx_len;
    uint32_t now_ms;
} UpdiEmu;

/** Reset the emulator and wire it into a transport vtable. */
void updi_emu_init(UpdiEmu* e, UpdiTransport* t);

/** Convenience presets for the two NVM variants we ship. */
void updi_emu_config_p0(UpdiEmu* e, const uint8_t sig[3], const char* family);
void updi_emu_config_p3(UpdiEmu* e, const uint8_t sig[3], const char* family);

/** Clear just the TX capture log (call before an op you want to inspect). */
void updi_emu_clear_tx(UpdiEmu* e);
