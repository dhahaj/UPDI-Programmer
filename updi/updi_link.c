/**
 * @file updi_link.c
 * @brief UPDI data-link + block read/write. Port of pymcuprog link.py / readwrite.py.
 *
 * Every instruction frame starts with the SYNCH byte (0x55) so the target can autobaud.
 * On the resistor-bridge wiring our TX echoes back on RX; that echo is consumed inside
 * transport->send(), so here recv() only ever yields genuine target bytes (ACKs / data).
 */
#include "updi_link.h"
#include "updi_constants.h"

#include <string.h>

#define UPDI_DEFAULT_TIMEOUT_MS 300u

#define ADDR24(link) ((link)->address_bytes == 3)

static inline bool send_raw(UpdiLink* link, const uint8_t* data, size_t len) {
    return link->transport->send(link->transport->ctx, data, len);
}

static inline size_t recv_raw(UpdiLink* link, uint8_t* data, size_t len) {
    return link->transport->recv(link->transport->ctx, data, len, link->timeout_ms);
}

/* Append the 2- or 3-byte little-endian address to a frame, return new length. */
static size_t put_address(UpdiLink* link, uint8_t* frame, size_t n, uint32_t address) {
    frame[n++] = (uint8_t)(address & 0xFF);
    frame[n++] = (uint8_t)((address >> 8) & 0xFF);
    if(ADDR24(link)) frame[n++] = (uint8_t)((address >> 16) & 0xFF);
    return n;
}

void updi_link_init(UpdiLink* link, const UpdiTransport* transport) {
    link->transport = transport;
    link->address_bytes = 2;
    link->timeout_ms = UPDI_DEFAULT_TIMEOUT_MS;
    link->ack_enabled = true;
}

void updi_link_set_address_size(UpdiLink* link, uint8_t address_bytes) {
    link->address_bytes = (address_bytes == 3) ? 3 : 2;
}

/* ---- Control/Status space ---- */

UpdiStatus updi_link_ldcs(UpdiLink* link, uint8_t address, uint8_t* out) {
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_LDCS | (address & 0x0F))};
    if(!send_raw(link, frame, 2)) return UpdiErrTransport;
    uint8_t resp;
    if(recv_raw(link, &resp, 1) != 1) return UpdiErrTimeout;
    if(out) *out = resp;
    return UpdiOk;
}

UpdiStatus updi_link_stcs(UpdiLink* link, uint8_t address, uint8_t value) {
    uint8_t frame[3] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_STCS | (address & 0x0F)), value};
    return send_raw(link, frame, 3) ? UpdiOk : UpdiErrTransport;
}

/* ACK on writes reduces latency tuning; RSD disables the response signature for
 * ACK-less block writes. Mirrors UpdiDatalink._enable_ack / _disable_ack. */
static UpdiStatus updi_link_enable_ack(UpdiLink* link) {
    UpdiStatus s = updi_link_stcs(link, UPDI_CS_CTRLA, (uint8_t)(1 << UPDI_CTRLA_IBDLY_BIT));
    if(s == UpdiOk) link->ack_enabled = true;
    return s;
}

static UpdiStatus updi_link_disable_ack(UpdiLink* link) {
    UpdiStatus s = updi_link_stcs(
        link, UPDI_CS_CTRLA, (uint8_t)((1 << UPDI_CTRLA_IBDLY_BIT) | (1 << UPDI_CTRLA_RSD_BIT)));
    if(s == UpdiOk) link->ack_enabled = false;
    return s;
}

void updi_link_double_break(UpdiLink* link) {
    if(link->transport->double_break) link->transport->double_break(link->transport->ctx);
    /* After a BREAK the device resets ASI registers; ACK defaults back on. */
    link->ack_enabled = true;
}

uint32_t updi_link_millis(UpdiLink* link) {
    if(link->transport->millis) return link->transport->millis(link->transport->ctx);
    return 0;
}

static UpdiStatus init_session_params(UpdiLink* link) {
    UpdiStatus s = updi_link_stcs(link, UPDI_CS_CTRLB, (uint8_t)(1 << UPDI_CTRLB_CCDETDIS_BIT));
    if(s != UpdiOk) return s;
    return updi_link_enable_ack(link);
}

static bool check_datalink(UpdiLink* link) {
    uint8_t statusa = 0;
    if(updi_link_ldcs(link, UPDI_CS_STATUSA, &statusa) != UpdiOk) return false;
    return statusa != 0; /* STATUSA non-zero => UPDI alive (carries the rev id) */
}

UpdiStatus updi_link_activate(UpdiLink* link) {
    /* Initial BREAK handshake (a 0x00 frame), as pymcuprog UpdiPhysical.__init__ does. */
    uint8_t brk = UPDI_BREAK;
    send_raw(link, &brk, 1);

    init_session_params(link);
    if(check_datalink(link)) return UpdiOk;

    /* Not responding — slam the state machine with a double BREAK and retry once. */
    updi_link_double_break(link);
    init_session_params(link);
    if(check_datalink(link)) return UpdiOk;

    return UpdiErrTimeout;
}

/* ---- direct single access ---- */

UpdiStatus updi_link_ld_byte(UpdiLink* link, uint32_t address, uint8_t* out) {
    uint8_t frame[5];
    size_t n = 0;
    frame[n++] = UPDI_PHY_SYNC;
    frame[n++] =
        (uint8_t)(UPDI_LDS | (ADDR24(link) ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16) | UPDI_DATA_8);
    n = put_address(link, frame, n, address);
    if(!send_raw(link, frame, n)) return UpdiErrTransport;
    uint8_t resp;
    if(recv_raw(link, &resp, 1) != 1) return UpdiErrTimeout;
    if(out) *out = resp;
    return UpdiOk;
}

/* Address phase already sent; complete the ACK/data/ACK handshake (pymcuprog _st_data_phase). */
static UpdiStatus st_data_phase(UpdiLink* link, const uint8_t* values, size_t n) {
    uint8_t ack;
    if(recv_raw(link, &ack, 1) != 1 || ack != UPDI_PHY_ACK) return UpdiErrNack;
    if(!send_raw(link, values, n)) return UpdiErrTransport;
    if(recv_raw(link, &ack, 1) != 1 || ack != UPDI_PHY_ACK) return UpdiErrNack;
    return UpdiOk;
}

UpdiStatus updi_link_st_byte(UpdiLink* link, uint32_t address, uint8_t value) {
    uint8_t frame[5];
    size_t n = 0;
    frame[n++] = UPDI_PHY_SYNC;
    frame[n++] =
        (uint8_t)(UPDI_STS | (ADDR24(link) ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16) | UPDI_DATA_8);
    n = put_address(link, frame, n, address);
    if(!send_raw(link, frame, n)) return UpdiErrTransport;
    return st_data_phase(link, &value, 1);
}

UpdiStatus updi_link_st16(UpdiLink* link, uint32_t address, uint16_t value) {
    uint8_t frame[5];
    size_t n = 0;
    frame[n++] = UPDI_PHY_SYNC;
    frame[n++] =
        (uint8_t)(UPDI_STS | (ADDR24(link) ? UPDI_ADDRESS_24 : UPDI_ADDRESS_16) | UPDI_DATA_16);
    n = put_address(link, frame, n, address);
    if(!send_raw(link, frame, n)) return UpdiErrTransport;
    uint8_t v[2] = {(uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF)};
    return st_data_phase(link, v, 2);
}

/* ---- pointer-based access ---- */

UpdiStatus updi_link_st_ptr(UpdiLink* link, uint32_t address) {
    uint8_t frame[5];
    size_t n = 0;
    frame[n++] = UPDI_PHY_SYNC;
    frame[n++] =
        (uint8_t)(UPDI_ST | UPDI_PTR_ADDRESS | (ADDR24(link) ? UPDI_DATA_24 : UPDI_DATA_16));
    n = put_address(link, frame, n, address);
    if(!send_raw(link, frame, n)) return UpdiErrTransport;
    uint8_t ack;
    if(recv_raw(link, &ack, 1) != 1 || ack != UPDI_PHY_ACK) return UpdiErrNack;
    return UpdiOk;
}

UpdiStatus updi_link_ld_ptr_inc(UpdiLink* link, uint8_t* buf, size_t size) {
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_LD | UPDI_PTR_INC | UPDI_DATA_8)};
    if(!send_raw(link, frame, 2)) return UpdiErrTransport;
    return (recv_raw(link, buf, size) == size) ? UpdiOk : UpdiErrTimeout;
}

UpdiStatus updi_link_ld_ptr_inc16(UpdiLink* link, uint8_t* buf, size_t words) {
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_LD | UPDI_PTR_INC | UPDI_DATA_16)};
    if(!send_raw(link, frame, 2)) return UpdiErrTransport;
    size_t want = words << 1;
    return (recv_raw(link, buf, want) == want) ? UpdiOk : UpdiErrTimeout;
}

UpdiStatus updi_link_st_ptr_inc(UpdiLink* link, const uint8_t* data, size_t size) {
    if(size == 0) return UpdiErrParam;
    uint8_t frame[3] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_ST | UPDI_PTR_INC | UPDI_DATA_8), data[0]};
    if(!send_raw(link, frame, 3)) return UpdiErrTransport;
    uint8_t ack;
    if(recv_raw(link, &ack, 1) != 1 || ack != UPDI_PHY_ACK) return UpdiErrNack;
    /* REPEAT auto-repeats the ST PTR++ instruction, so we just stream raw data bytes. */
    for(size_t i = 1; i < size; i++) {
        if(!send_raw(link, &data[i], 1)) return UpdiErrTransport;
        if(recv_raw(link, &ack, 1) != 1 || ack != UPDI_PHY_ACK) return UpdiErrNack;
    }
    return UpdiOk;
}

UpdiStatus updi_link_st_ptr_inc16_block(UpdiLink* link, const uint8_t* data, size_t len) {
    /* ACK-less block write (pymcuprog st_ptr_inc16 else-branch): disable RSD, send the
     * ST16 PTR++ opcode, stream the whole block with no per-word ACK, re-enable ACK. */
    UpdiStatus s = updi_link_disable_ack(link);
    if(s != UpdiOk) return s;
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_ST | UPDI_PTR_INC | UPDI_DATA_16)};
    if(!send_raw(link, frame, 2)) return UpdiErrTransport;
    if(!send_raw(link, data, len)) return UpdiErrTransport;
    return updi_link_enable_ack(link);
}

/* ---- misc instructions ---- */

UpdiStatus updi_link_repeat(UpdiLink* link, uint16_t count) {
    if(count < 1 || count > UPDI_MAX_REPEAT_SIZE) return UpdiErrParam;
    uint16_t r = (uint16_t)(count - 1);
    uint8_t frame[3] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_REPEAT | UPDI_REPEAT_BYTE), (uint8_t)(r & 0xFF)};
    return send_raw(link, frame, 3) ? UpdiOk : UpdiErrTransport;
}

UpdiStatus updi_link_key(UpdiLink* link, uint8_t size_idx, const char* key, size_t key_len) {
    /* Key length must be 8 << size_idx (64/128/256-bit). Sent reversed on the wire. */
    if(key_len != (size_t)(8u << size_idx)) return UpdiErrParam;
    if(key_len > 32) return UpdiErrParam;
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_KEY | UPDI_KEY_KEY | size_idx)};
    if(!send_raw(link, frame, 2)) return UpdiErrTransport;
    uint8_t rev[32];
    for(size_t i = 0; i < key_len; i++) rev[i] = (uint8_t)key[key_len - 1 - i];
    return send_raw(link, rev, key_len) ? UpdiOk : UpdiErrTransport;
}

UpdiStatus updi_link_read_sib(UpdiLink* link, uint8_t* buf, size_t size, size_t* out_len) {
    uint8_t sel;
    if(size >= 32)
        sel = UPDI_SIB_32BYTES;
    else if(size >= 16)
        sel = UPDI_SIB_16BYTES;
    else
        sel = UPDI_SIB_8BYTES;
    uint8_t frame[2] = {UPDI_PHY_SYNC, (uint8_t)(UPDI_KEY | UPDI_KEY_SIB | sel)};
    if(!send_raw(link, frame, 2)) return UpdiErrTransport;
    size_t got = recv_raw(link, buf, size);
    if(out_len) *out_len = got;
    return (got > 0) ? UpdiOk : UpdiErrTimeout;
}

/* ---- high-level block read/write (readwrite.py) ---- */

UpdiStatus updi_link_read_data(UpdiLink* link, uint32_t address, uint8_t* buf, size_t size) {
    if(size > UPDI_MAX_REPEAT_SIZE) return UpdiErrParam;
    UpdiStatus s = updi_link_st_ptr(link, address);
    if(s != UpdiOk) return s;
    if(size > 1) {
        s = updi_link_repeat(link, (uint16_t)size);
        if(s != UpdiOk) return s;
    }
    return updi_link_ld_ptr_inc(link, buf, size);
}

UpdiStatus updi_link_read_data_words(UpdiLink* link, uint32_t address, uint8_t* buf, size_t words) {
    if(words > (UPDI_MAX_REPEAT_SIZE >> 1)) return UpdiErrParam;
    UpdiStatus s = updi_link_st_ptr(link, address);
    if(s != UpdiOk) return s;
    if(words > 1) {
        s = updi_link_repeat(link, (uint16_t)words);
        if(s != UpdiOk) return s;
    }
    return updi_link_ld_ptr_inc16(link, buf, words);
}

UpdiStatus
    updi_link_write_data_words(UpdiLink* link, uint32_t address, const uint8_t* data, size_t numbytes) {
    if(numbytes == 0 || (numbytes & 1)) return UpdiErrParam;
    if(numbytes == 2) {
        uint16_t value = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
        return updi_link_st16(link, address, value);
    }
    if(numbytes > (UPDI_MAX_REPEAT_SIZE << 1)) return UpdiErrParam;
    UpdiStatus s = updi_link_st_ptr(link, address);
    if(s != UpdiOk) return s;
    s = updi_link_repeat(link, (uint16_t)(numbytes >> 1));
    if(s != UpdiOk) return s;
    return updi_link_st_ptr_inc16_block(link, data, numbytes);
}

UpdiStatus updi_link_write_data(UpdiLink* link, uint32_t address, const uint8_t* data, size_t numbytes) {
    if(numbytes == 0) return UpdiErrParam;
    if(numbytes == 1) return updi_link_st_byte(link, address, data[0]);
    if(numbytes == 2) {
        UpdiStatus s = updi_link_st_byte(link, address, data[0]);
        if(s != UpdiOk) return s;
        return updi_link_st_byte(link, address + 1, data[1]);
    }
    size_t index = 0;
    while(numbytes) {
        size_t chunk = (numbytes > UPDI_MAX_REPEAT_SIZE) ? UPDI_MAX_REPEAT_SIZE : numbytes;
        UpdiStatus s = updi_link_st_ptr(link, address);
        if(s != UpdiOk) return s;
        s = updi_link_repeat(link, (uint16_t)chunk);
        if(s != UpdiOk) return s;
        s = updi_link_st_ptr_inc(link, &data[index], chunk);
        if(s != UpdiOk) return s;
        index += chunk;
        address += chunk;
        numbytes -= chunk;
    }
    return UpdiOk;
}
