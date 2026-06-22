/**
 * @file updi_session.h
 * @brief High-level UPDI programming session: connect/identify, erase, flash-image
 *        write/verify, dump, and fuses — with progress callbacks.
 *
 * Operates on in-RAM buffers only (no files), so it stays portable and host-testable.
 * The Flipper app layers SD-card file I/O (Intel HEX) on top of this.
 */
#pragma once

#include "updi_link.h"
#include "updi_nvm.h"
#include "updi_device.h"

typedef void (*UpdiProgressCb)(void* ctx, size_t done, size_t total);

/** Details of the first byte that differed during a verify (filled on UpdiErrVerify). */
typedef struct {
    size_t offset; /* 0-based flash offset of the first differing byte */
    uint8_t expected; /* byte from the supplied image (HEX) */
    uint8_t actual; /* byte read back from the device */
} UpdiVerifyMismatch;

typedef struct {
    UpdiLink link;
    UpdiNvm nvm;
    const UpdiDevice* device; /* NULL until identified */
    UpdiSibInfo sib;
    uint8_t sig[3];
    bool connected; /* true once in programming mode with a known device */
} UpdiSession;

void updi_session_init(UpdiSession* s, const UpdiTransport* transport);

/**
 * Activate UPDI, read the SIB, choose the address size, enter programming mode, read the
 * signature and identify the device. Returns:
 *   UpdiOk          - connected, device known (use updi_session_device)
 *   UpdiErrLocked   - device is locked; call updi_session_unlock() then connect() again
 *   UpdiErrUnsupported - signature not in the device table
 *   other           - transport/protocol failure
 */
UpdiStatus updi_session_connect(UpdiSession* s);

/** Unlock a locked device via chip-erase KEY (erases flash/eeprom). Then re-connect. */
UpdiStatus updi_session_unlock(UpdiSession* s);

/** Leave programming mode (releases the target from reset). */
void updi_session_disconnect(UpdiSession* s);

const UpdiDevice* updi_session_device(const UpdiSession* s);
const UpdiSibInfo* updi_session_sib(const UpdiSession* s);
const uint8_t* updi_session_signature(const UpdiSession* s);

/** Chip erase via the NVM controller (device must be connected/unlocked). */
UpdiStatus updi_session_chip_erase(UpdiSession* s);

/**
 * Program @p len bytes (image starting at flash base) page by page. Pages that are
 * entirely 0xFF are skipped (already erased). Assumes a preceding chip/flash erase.
 */
UpdiStatus updi_session_write_flash(
    UpdiSession* s,
    const uint8_t* data,
    size_t len,
    UpdiProgressCb cb,
    void* cb_ctx);

/**
 * Read back and compare @p len bytes against @p data; UpdiErrVerify on mismatch.
 * If @p mismatch is non-NULL it is filled with the first differing byte's location and
 * values when UpdiErrVerify is returned.
 */
UpdiStatus updi_session_verify_flash(
    UpdiSession* s,
    const uint8_t* data,
    size_t len,
    UpdiProgressCb cb,
    void* cb_ctx,
    UpdiVerifyMismatch* mismatch);

/** Read @p len bytes of flash (from base) into @p buf. */
UpdiStatus
    updi_session_read_flash(UpdiSession* s, uint8_t* buf, size_t len, UpdiProgressCb cb, void* cb_ctx);

/** Read @p count fuse bytes into @p buf (clamped to the device's fuse count). */
UpdiStatus updi_session_read_fuses(UpdiSession* s, uint8_t* buf, size_t count);

/** Write one fuse byte. */
UpdiStatus updi_session_write_fuse(UpdiSession* s, uint8_t index, uint8_t value);
