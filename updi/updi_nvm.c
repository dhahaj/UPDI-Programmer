/**
 * @file updi_nvm.c
 * @brief UPDI NVM programming. Ports application.py (progmode/keys/reset), nvmp0.py and
 *        nvmp3.py (NVMCTRL command sets). The two NVM variants are captured in a small
 *        descriptor table so the page/erase/fuse flow is written once.
 */
#include "updi_nvm.h"
#include "updi_constants.h"

#include <string.h>

/* Safety backstop for the poll loops in case the transport supplies no real clock. */
#define NVM_MAX_POLL_ITERS 200000u

/* Per-variant NVMCTRL register offsets, command codes and status masks. */
typedef struct {
    uint8_t reg_ctrla; /* command register offset from nvmctrl_base */
    uint8_t reg_status;
    uint8_t reg_data; /* P:0 fuse-write data register */
    uint8_t reg_addr; /* P:0 fuse-write address register */

    uint8_t cmd_chip_erase;
    uint8_t cmd_page_buffer_clear; /* flash page buffer clear */
    uint8_t cmd_write_page; /* flash page write (pre-erased) */
    uint8_t cmd_erase_page; /* flash page erase */
    uint8_t cmd_write_fuse; /* P:0 only */
    uint8_t cmd_eeprom_buffer_clear; /* P:3 fuse-as-eeprom path */
    uint8_t cmd_eeprom_erase_write; /* P:3 fuse-as-eeprom path */
    uint8_t cmd_nocmd; /* command-clear value (P:3) */

    uint8_t status_wrerr_mask;
    uint8_t status_busy_mask; /* EEPROM busy | flash busy */

    bool clear_cmd_after; /* P:3 must write NOCMD after each command */
    bool fuse_via_eeprom; /* P:3 writes fuses through the EEPROM page path */
} NvmVariantDesc;

/* P:0 — tinyAVR 0/1/2, megaAVR 0 (nvmp0.py) */
static const NvmVariantDesc nvm_desc_p0 = {
    .reg_ctrla = 0x00,
    .reg_status = 0x02,
    .reg_data = 0x06,
    .reg_addr = 0x08,
    .cmd_chip_erase = 0x05,
    .cmd_page_buffer_clear = 0x04,
    .cmd_write_page = 0x01,
    .cmd_erase_page = 0x02,
    .cmd_write_fuse = 0x07,
    .cmd_eeprom_buffer_clear = 0x00,
    .cmd_eeprom_erase_write = 0x00,
    .cmd_nocmd = 0x00,
    .status_wrerr_mask = (1 << 2),
    .status_busy_mask = (1 << 1) | (1 << 0),
    .clear_cmd_after = false,
    .fuse_via_eeprom = false,
};

/* P:3 — AVR EA (nvmp3.py) */
static const NvmVariantDesc nvm_desc_p3 = {
    .reg_ctrla = 0x00,
    .reg_status = 0x06,
    .reg_data = 0x08,
    .reg_addr = 0x0C,
    .cmd_chip_erase = 0x20,
    .cmd_page_buffer_clear = 0x0F, /* FLASH_PAGE_BUFFER_CLEAR */
    .cmd_write_page = 0x04, /* FLASH_PAGE_WRITE */
    .cmd_erase_page = 0x08, /* FLASH_PAGE_ERASE */
    .cmd_write_fuse = 0xFF, /* N/A — fuses go via the EEPROM path */
    .cmd_eeprom_buffer_clear = 0x1F, /* EEPROM_PAGE_BUFFER_CLEAR */
    .cmd_eeprom_erase_write = 0x15, /* EEPROM_PAGE_ERASE_WRITE */
    .cmd_nocmd = 0x00,
    .status_wrerr_mask = 0x70,
    .status_busy_mask = (1 << 0) | (1 << 1),
    .clear_cmd_after = true,
    .fuse_via_eeprom = true,
};

static const NvmVariantDesc* nvm_desc(const UpdiNvm* nvm) {
    return (nvm->device->nvm_variant == UpdiNvmVariantP3) ? &nvm_desc_p3 : &nvm_desc_p0;
}

void updi_nvm_init(UpdiNvm* nvm, UpdiLink* link, const UpdiDevice* device) {
    nvm->link = link;
    nvm->device = device;
}

void updi_nvm_set_device(UpdiNvm* nvm, const UpdiDevice* device) {
    nvm->device = device;
}

/* ---- low-level CS/ASI helpers ---- */

static UpdiStatus nvm_reset(UpdiNvm* nvm, bool apply) {
    return updi_link_stcs(
        nvm->link, UPDI_ASI_RESET_REQ, apply ? UPDI_RESET_REQ_VALUE : 0x00);
}

bool updi_nvm_in_progmode(UpdiNvm* nvm) {
    uint8_t st = 0;
    if(updi_link_ldcs(nvm->link, UPDI_ASI_SYS_STATUS, &st) != UpdiOk) return false;
    return (st & (1 << UPDI_ASI_SYS_STATUS_NVMPROG)) != 0;
}

bool updi_nvm_is_locked(UpdiNvm* nvm) {
    uint8_t st = 0;
    if(updi_link_ldcs(nvm->link, UPDI_ASI_SYS_STATUS, &st) != UpdiOk) return false;
    return (st & (1 << UPDI_ASI_SYS_STATUS_LOCKSTATUS)) != 0;
}

static bool wait_unlocked(UpdiNvm* nvm, uint32_t timeout_ms) {
    uint32_t start = updi_link_millis(nvm->link);
    uint32_t iters = 0;
    for(;;) {
        uint8_t st = 0;
        if(updi_link_ldcs(nvm->link, UPDI_ASI_SYS_STATUS, &st) == UpdiOk) {
            if(!(st & (1 << UPDI_ASI_SYS_STATUS_LOCKSTATUS))) return true;
        }
        if((uint32_t)(updi_link_millis(nvm->link) - start) >= timeout_ms) return false;
        if(++iters >= NVM_MAX_POLL_ITERS) return false;
    }
}

/* ---- programming mode ---- */

UpdiStatus updi_nvm_enter_progmode(UpdiNvm* nvm) {
    if(updi_nvm_in_progmode(nvm)) return UpdiOk;

    /* Hold the part in reset while we hand over the NVMPROG key. */
    UpdiStatus s = nvm_reset(nvm, true);
    if(s != UpdiOk) return s;

    s = updi_link_key(nvm->link, UPDI_KEY_64, UPDI_KEY_NVM, UPDI_KEY_LEN);
    if(s != UpdiOk) return s;

    uint8_t key_status = 0;
    s = updi_link_ldcs(nvm->link, UPDI_ASI_KEY_STATUS, &key_status);
    if(s != UpdiOk) return s;
    if(!(key_status & (1 << UPDI_ASI_KEY_STATUS_NVMPROG))) return UpdiErrProtocol; /* key rejected */

    /* Toggle reset to apply the key. */
    nvm_reset(nvm, true);
    nvm_reset(nvm, false);

    if(!wait_unlocked(nvm, 100)) return UpdiErrLocked; /* locked device */
    if(!updi_nvm_in_progmode(nvm)) return UpdiErrProtocol;
    return UpdiOk;
}

UpdiStatus updi_nvm_leave_progmode(UpdiNvm* nvm) {
    nvm_reset(nvm, true);
    nvm_reset(nvm, false);
    /* Disable UPDI (releases keys) and keep collision-detect disabled. */
    return updi_link_stcs(
        nvm->link,
        UPDI_CS_CTRLB,
        (uint8_t)((1 << UPDI_CTRLB_UPDIDIS_BIT) | (1 << UPDI_CTRLB_CCDETDIS_BIT)));
}

UpdiStatus updi_nvm_unlock(UpdiNvm* nvm) {
    /* Unlock by chip erase using the dedicated KEY (works on a locked device). */
    UpdiStatus s = updi_link_key(nvm->link, UPDI_KEY_64, UPDI_KEY_CHIPERASE, UPDI_KEY_LEN);
    if(s != UpdiOk) return s;

    uint8_t key_status = 0;
    s = updi_link_ldcs(nvm->link, UPDI_ASI_KEY_STATUS, &key_status);
    if(s != UpdiOk) return s;
    if(!(key_status & (1 << UPDI_ASI_KEY_STATUS_CHIPERASE))) return UpdiErrProtocol;

    nvm_reset(nvm, true);
    nvm_reset(nvm, false);

    if(!wait_unlocked(nvm, 500)) return UpdiErrNvm;
    return UpdiOk;
}

/* ---- identification ---- */

UpdiStatus updi_nvm_read_sib_info(UpdiLink* link, UpdiSibInfo* info) {
    uint8_t sib[16];
    size_t got = 0;
    UpdiStatus s = updi_link_read_sib(link, sib, sizeof(sib), &got);
    if(s != UpdiOk) return s;
    if(got < 11) return UpdiErrProtocol; /* need at least up to the "P:x" field */

    memset(info, 0, sizeof(*info));
    /* Family = bytes [0:7], trimmed of trailing spaces. */
    size_t fam_len = 7;
    while(fam_len > 0 && (sib[fam_len - 1] == ' ' || sib[fam_len - 1] == 0)) fam_len--;
    memcpy(info->family, sib, fam_len);
    info->family[fam_len] = '\0';

    /* NVM interface version: the digit in "P:x" which sits at byte 10. */
    uint8_t d = sib[10];
    info->nvm_version = (d >= '0' && d <= '9') ? (uint8_t)(d - '0') : 0xFF;
    return UpdiOk;
}

UpdiStatus updi_nvm_read_signature(UpdiNvm* nvm, uint8_t sig[3]) {
    uint32_t addr = nvm->device ? nvm->device->sigrow_address : UPDI_SIGROW_DEFAULT_ADDRESS;
    return updi_link_read_data(nvm->link, addr, sig, 3);
}

UpdiStatus updi_nvm_read_revision(UpdiNvm* nvm, uint8_t* out) {
    return updi_link_ld_byte(nvm->link, nvm->device->syscfg_base + 1, out);
}

/* ---- NVMCTRL command primitives ---- */

static UpdiStatus execute_nvm_command(UpdiNvm* nvm, uint8_t command) {
    const NvmVariantDesc* d = nvm_desc(nvm);
    return updi_link_st_byte(nvm->link, nvm->device->nvmctrl_base + d->reg_ctrla, command);
}

static UpdiStatus wait_nvm_ready(UpdiNvm* nvm, uint32_t timeout_ms) {
    const NvmVariantDesc* d = nvm_desc(nvm);
    uint32_t start = updi_link_millis(nvm->link);
    uint32_t iters = 0;
    for(;;) {
        uint8_t status = 0;
        UpdiStatus s =
            updi_link_ld_byte(nvm->link, nvm->device->nvmctrl_base + d->reg_status, &status);
        if(s != UpdiOk) return s;
        if(status & d->status_wrerr_mask) return UpdiErrNvm;
        if(!(status & d->status_busy_mask)) return UpdiOk;
        if((uint32_t)(updi_link_millis(nvm->link) - start) >= timeout_ms) return UpdiErrTimeout;
        if(++iters >= NVM_MAX_POLL_ITERS) return UpdiErrTimeout;
    }
}

/* ---- erase ---- */

UpdiStatus updi_nvm_chip_erase(UpdiNvm* nvm) {
    const NvmVariantDesc* d = nvm_desc(nvm);
    UpdiStatus s = wait_nvm_ready(nvm, 100);
    if(s != UpdiOk) return s;
    s = execute_nvm_command(nvm, d->cmd_chip_erase);
    if(s != UpdiOk) return s;
    UpdiStatus ready = wait_nvm_ready(nvm, 500);
    if(d->clear_cmd_after) execute_nvm_command(nvm, d->cmd_nocmd);
    return ready;
}

UpdiStatus updi_nvm_erase_flash_page(UpdiNvm* nvm, uint32_t address) {
    const NvmVariantDesc* d = nvm_desc(nvm);
    UpdiStatus s = wait_nvm_ready(nvm, 100);
    if(s != UpdiOk) return s;
    /* A dummy write to the page selects it for the erase command. */
    uint8_t dummy = 0xFF;
    s = updi_link_write_data(nvm->link, address, &dummy, 1);
    if(s != UpdiOk) return s;
    s = execute_nvm_command(nvm, d->cmd_erase_page);
    if(s != UpdiOk) return s;
    UpdiStatus ready = wait_nvm_ready(nvm, 100);
    if(d->clear_cmd_after) execute_nvm_command(nvm, d->cmd_nocmd);
    return ready;
}

/* ---- read ---- */

UpdiStatus updi_nvm_read(UpdiNvm* nvm, uint32_t address, uint8_t* buf, size_t size) {
    size_t off = 0;
    while(size) {
        size_t chunk = (size > UPDI_MAX_REPEAT_SIZE) ? UPDI_MAX_REPEAT_SIZE : size;
        UpdiStatus s = updi_link_read_data(nvm->link, address + off, buf + off, chunk);
        if(s != UpdiOk) return s;
        off += chunk;
        size -= chunk;
    }
    return UpdiOk;
}

/* ---- write ---- */

UpdiStatus
    updi_nvm_write_flash_page(UpdiNvm* nvm, uint32_t address, const uint8_t* data, size_t len) {
    const NvmVariantDesc* d = nvm_desc(nvm);
    if(len == 0 || (len & 1)) return UpdiErrParam; /* word access */
    if(len > nvm->device->flash_page_size) return UpdiErrParam;

    UpdiStatus s = wait_nvm_ready(nvm, 100);
    if(s != UpdiOk) return s;

    /* Clear the page buffer, then fill it by writing words straight to the flash address. */
    s = execute_nvm_command(nvm, d->cmd_page_buffer_clear);
    if(s != UpdiOk) return s;
    s = wait_nvm_ready(nvm, 100);
    if(s != UpdiOk) return s;

    s = updi_link_write_data_words(nvm->link, address, data, len);
    if(s != UpdiOk) return s;

    /* Commit (page already erased, e.g. by a preceding chip erase). */
    s = execute_nvm_command(nvm, d->cmd_write_page);
    if(s != UpdiOk) return s;
    UpdiStatus ready = wait_nvm_ready(nvm, 100);
    if(d->clear_cmd_after) execute_nvm_command(nvm, d->cmd_nocmd);
    return ready;
}

/* ---- fuses ---- */

UpdiStatus updi_nvm_read_fuse(UpdiNvm* nvm, uint8_t index, uint8_t* out) {
    if(index >= nvm->device->fuses_size) return UpdiErrParam;
    return updi_link_ld_byte(nvm->link, nvm->device->fuses_address + index, out);
}

UpdiStatus updi_nvm_write_fuse(UpdiNvm* nvm, uint8_t index, uint8_t value) {
    const NvmVariantDesc* d = nvm_desc(nvm);
    if(index >= nvm->device->fuses_size) return UpdiErrParam;
    uint32_t addr = nvm->device->fuses_address + index;

    if(d->fuse_via_eeprom) {
        /* P:3: a fuse is written through the EEPROM page-erase-write path (nvmp3.py). */
        UpdiStatus s = wait_nvm_ready(nvm, 100);
        if(s != UpdiOk) return s;
        s = execute_nvm_command(nvm, d->cmd_eeprom_buffer_clear);
        if(s != UpdiOk) return s;
        s = wait_nvm_ready(nvm, 100);
        if(s != UpdiOk) return s;
        s = updi_link_write_data(nvm->link, addr, &value, 1);
        if(s != UpdiOk) return s;
        s = execute_nvm_command(nvm, d->cmd_eeprom_erase_write);
        if(s != UpdiOk) return s;
        UpdiStatus ready = wait_nvm_ready(nvm, 100);
        execute_nvm_command(nvm, d->cmd_nocmd);
        return ready;
    }

    /* P:0: load NVMCTRL ADDR (16-bit) + DATA, then issue WRITE_FUSE (nvmp0.py). */
    UpdiStatus s = wait_nvm_ready(nvm, 100);
    if(s != UpdiOk) return s;
    s = updi_link_st_byte(nvm->link, nvm->device->nvmctrl_base + d->reg_addr, (uint8_t)(addr & 0xFF));
    if(s != UpdiOk) return s;
    s = updi_link_st_byte(
        nvm->link, nvm->device->nvmctrl_base + d->reg_addr + 1, (uint8_t)((addr >> 8) & 0xFF));
    if(s != UpdiOk) return s;
    s = updi_link_st_byte(nvm->link, nvm->device->nvmctrl_base + d->reg_data, value);
    if(s != UpdiOk) return s;
    s = execute_nvm_command(nvm, d->cmd_write_fuse);
    if(s != UpdiOk) return s;
    return wait_nvm_ready(nvm, 100);
}
