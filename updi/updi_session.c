/**
 * @file updi_session.c
 * @brief High-level UPDI session orchestration. See updi_session.h.
 */
#include "updi_session.h"

#include <string.h>

#define UPDI_MAX_PAGE 256u
#define UPDI_READ_CHUNK 256u

void updi_session_init(UpdiSession* s, const UpdiTransport* transport) {
    memset(s, 0, sizeof(*s));
    updi_link_init(&s->link, transport);
    updi_nvm_init(&s->nvm, &s->link, NULL);
}

UpdiStatus updi_session_connect(UpdiSession* s) {
    s->connected = false;
    s->device = NULL;

    UpdiStatus st = updi_link_activate(&s->link);
    if(st != UpdiOk) return st;

    st = updi_nvm_read_sib_info(&s->link, &s->sib);
    if(st != UpdiOk) return st;

    /* Address size follows the NVM interface: 16-bit for P:0, 24-bit for the rest. */
    updi_link_set_address_size(&s->link, (s->sib.nvm_version == 0) ? 2 : 3);

    updi_nvm_init(&s->nvm, &s->link, NULL);
    st = updi_nvm_enter_progmode(&s->nvm);
    if(st != UpdiOk) return st; /* may be UpdiErrLocked */

    st = updi_nvm_read_signature(&s->nvm, s->sig);
    if(st != UpdiOk) return st;

    const UpdiDevice* dev = updi_device_find_by_id(updi_device_id_from_sig(s->sig));
    if(!dev) return UpdiErrUnsupported;

    updi_nvm_set_device(&s->nvm, dev);
    s->device = dev;
    s->connected = true;
    return UpdiOk;
}

UpdiStatus updi_session_unlock(UpdiSession* s) {
    return updi_nvm_unlock(&s->nvm);
}

void updi_session_disconnect(UpdiSession* s) {
    updi_nvm_leave_progmode(&s->nvm);
    s->connected = false;
}

const UpdiDevice* updi_session_device(const UpdiSession* s) {
    return s->device;
}
const UpdiSibInfo* updi_session_sib(const UpdiSession* s) {
    return &s->sib;
}
const uint8_t* updi_session_signature(const UpdiSession* s) {
    return s->sig;
}

UpdiStatus updi_session_chip_erase(UpdiSession* s) {
    if(!s->device) return UpdiErrParam;
    return updi_nvm_chip_erase(&s->nvm);
}

static bool all_ff(const uint8_t* p, size_t n) {
    for(size_t i = 0; i < n; i++)
        if(p[i] != 0xFF) return false;
    return true;
}

UpdiStatus updi_session_write_flash(
    UpdiSession* s,
    const uint8_t* data,
    size_t len,
    UpdiProgressCb cb,
    void* cb_ctx) {
    if(!s->connected || !s->device) return UpdiErrParam;
    size_t page = s->device->flash_page_size;
    if(page == 0 || page > UPDI_MAX_PAGE) return UpdiErrParam;

    uint8_t pagebuf[UPDI_MAX_PAGE];
    for(size_t off = 0; off < len; off += page) {
        size_t chunk = (len - off < page) ? (len - off) : page;
        memset(pagebuf, 0xFF, page);
        memcpy(pagebuf, data + off, chunk);
        if(!all_ff(pagebuf, page)) {
            UpdiStatus st =
                updi_nvm_write_flash_page(&s->nvm, s->device->flash_address + off, pagebuf, page);
            if(st != UpdiOk) return st;
        }
        if(cb) cb(cb_ctx, (off + chunk), len);
    }
    return UpdiOk;
}

UpdiStatus updi_session_verify_flash(
    UpdiSession* s,
    const uint8_t* data,
    size_t len,
    UpdiProgressCb cb,
    void* cb_ctx,
    UpdiVerifyMismatch* mismatch) {
    if(!s->connected || !s->device) return UpdiErrParam;
    uint8_t rb[UPDI_READ_CHUNK];
    size_t off = 0;
    while(off < len) {
        size_t chunk = (len - off < UPDI_READ_CHUNK) ? (len - off) : UPDI_READ_CHUNK;
        UpdiStatus st = updi_nvm_read(&s->nvm, s->device->flash_address + off, rb, chunk);
        if(st != UpdiOk) return st;
        for(size_t i = 0; i < chunk; i++) {
            if(rb[i] != data[off + i]) {
                if(mismatch) {
                    mismatch->offset = off + i;
                    mismatch->expected = data[off + i];
                    mismatch->actual = rb[i];
                }
                return UpdiErrVerify;
            }
        }
        off += chunk;
        if(cb) cb(cb_ctx, off, len);
    }
    return UpdiOk;
}

UpdiStatus updi_session_read_flash(
    UpdiSession* s,
    uint8_t* buf,
    size_t len,
    UpdiProgressCb cb,
    void* cb_ctx) {
    if(!s->connected || !s->device) return UpdiErrParam;
    size_t off = 0;
    while(off < len) {
        size_t chunk = (len - off < UPDI_READ_CHUNK) ? (len - off) : UPDI_READ_CHUNK;
        UpdiStatus st = updi_nvm_read(&s->nvm, s->device->flash_address + off, buf + off, chunk);
        if(st != UpdiOk) return st;
        off += chunk;
        if(cb) cb(cb_ctx, off, len);
    }
    return UpdiOk;
}

UpdiStatus updi_session_read_fuses(UpdiSession* s, uint8_t* buf, size_t count) {
    if(!s->connected || !s->device) return UpdiErrParam;
    if(count > s->device->fuses_size) count = s->device->fuses_size;
    for(size_t i = 0; i < count; i++) {
        UpdiStatus st = updi_nvm_read_fuse(&s->nvm, (uint8_t)i, &buf[i]);
        if(st != UpdiOk) return st;
    }
    return UpdiOk;
}

UpdiStatus updi_session_write_fuse(UpdiSession* s, uint8_t index, uint8_t value) {
    if(!s->connected || !s->device) return UpdiErrParam;
    return updi_nvm_write_fuse(&s->nvm, index, value);
}
