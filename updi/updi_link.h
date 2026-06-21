/**
 * @file updi_link.h
 * @brief UPDI data-link layer: SYNCH framing, LDCS/STCS, LDS/STS, LD/ST, REPEAT, KEY,
 *        SIB read, plus the block read/write helpers (the pymcuprog "readwrite" layer).
 *
 * Ported from pymcuprog/serialupdi/link.py and readwrite.py. Pure protocol: depends only
 * on updi_transport.h. Supports both 16-bit (NVM P:0) and 24-bit (NVM P:2/3/4/5)
 * addressing via updi_link_set_address_size().
 */
#pragma once

#include "updi_transport.h"

/** Result codes used across the whole updi/ stack. */
typedef enum {
    UpdiOk = 0,
    UpdiErrTimeout, /* no/short response within timeout */
    UpdiErrNack, /* expected ACK (0x40) not received */
    UpdiErrProtocol, /* malformed/unexpected response */
    UpdiErrParam, /* bad argument */
    UpdiErrVerify, /* read-back mismatch */
    UpdiErrNvm, /* NVMCTRL reported a write error */
    UpdiErrLocked, /* device is locked */
    UpdiErrUnsupported, /* unsupported device / NVM variant */
    UpdiErrTransport, /* transport send failed */
} UpdiStatus;

typedef struct {
    const UpdiTransport* transport;
    uint8_t address_bytes; /* 2 = 16-bit DL, 3 = 24-bit DL */
    uint32_t timeout_ms; /* default per-response timeout */
    bool ack_enabled; /* mirrors the device CTRLA RSD state we last set */
} UpdiLink;

/* ---- lifecycle ---- */

/** Initialise the link struct around a transport. Defaults to 16-bit addressing. */
void updi_link_init(UpdiLink* link, const UpdiTransport* transport);

/** Select 2 (16-bit) or 3 (24-bit) address bytes. */
void updi_link_set_address_size(UpdiLink* link, uint8_t address_bytes);

/**
 * Bring UPDI to a known, active state: set CTRLB.CCDETDIS, enable ACKs, and verify by
 * reading STATUSA. On failure, send a double BREAK and retry once. Mirrors
 * UpdiDatalink.init_datalink().
 */
UpdiStatus updi_link_activate(UpdiLink* link);

/* ---- Control/Status space ---- */
UpdiStatus updi_link_ldcs(UpdiLink* link, uint8_t address, uint8_t* out);
UpdiStatus updi_link_stcs(UpdiLink* link, uint8_t address, uint8_t value);

/* ---- direct (addressed) single access ---- */
UpdiStatus updi_link_ld_byte(UpdiLink* link, uint32_t address, uint8_t* out);
UpdiStatus updi_link_st_byte(UpdiLink* link, uint32_t address, uint8_t value);
UpdiStatus updi_link_st16(UpdiLink* link, uint32_t address, uint16_t value);

/* ---- pointer-based access ---- */
UpdiStatus updi_link_st_ptr(UpdiLink* link, uint32_t address);
UpdiStatus updi_link_ld_ptr_inc(UpdiLink* link, uint8_t* buf, size_t size);
UpdiStatus updi_link_ld_ptr_inc16(UpdiLink* link, uint8_t* buf, size_t words);
UpdiStatus updi_link_st_ptr_inc(UpdiLink* link, const uint8_t* data, size_t size);
UpdiStatus updi_link_st_ptr_inc16_block(UpdiLink* link, const uint8_t* data, size_t len);

/* ---- misc instructions ---- */
UpdiStatus updi_link_repeat(UpdiLink* link, uint16_t count);
UpdiStatus updi_link_key(UpdiLink* link, uint8_t size_idx, const char* key, size_t key_len);
UpdiStatus updi_link_read_sib(UpdiLink* link, uint8_t* buf, size_t size, size_t* out_len);

/* ---- high-level block read/write (readwrite.py) ---- */
UpdiStatus updi_link_read_data(UpdiLink* link, uint32_t address, uint8_t* buf, size_t size);
UpdiStatus
    updi_link_read_data_words(UpdiLink* link, uint32_t address, uint8_t* buf, size_t words);
UpdiStatus updi_link_write_data(UpdiLink* link, uint32_t address, const uint8_t* data, size_t size);
UpdiStatus
    updi_link_write_data_words(UpdiLink* link, uint32_t address, const uint8_t* data, size_t size);

/* ---- BREAK recovery ---- */
void updi_link_double_break(UpdiLink* link);

/* ---- timing ---- */
/** Read the transport's millisecond counter (0 if the transport has no clock). */
uint32_t updi_link_millis(UpdiLink* link);
