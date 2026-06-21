/**
 * @file updi_emu.c
 * @brief UPDI target emulator (mock transport). See updi_emu.h.
 */
#include "updi_emu.h"
#include "../updi/updi_constants.h"

#include <string.h>

enum {
    S_IDLE = 0,
    S_OPCODE,
    S_STCS_VAL,
    S_REPEAT_VAL,
    S_LDS_ADDR,
    S_STS_ADDR,
    S_STS_DATA,
    S_STPTR_ADDR,
    S_STINC8,
    S_STINC16,
    S_KEY,
};

static void rx_push(UpdiEmu* e, uint8_t b) {
    size_t next = (e->rx_tail + 1) % EMU_RX_CAP;
    if(next == e->rx_head) return; /* full: drop (test should not overflow) */
    e->rx[e->rx_tail] = b;
    e->rx_tail = next;
}

static int rx_pop(UpdiEmu* e, uint8_t* b) {
    if(e->rx_head == e->rx_tail) return 0;
    *b = e->rx[e->rx_head];
    e->rx_head = (e->rx_head + 1) % EMU_RX_CAP;
    return 1;
}

/* ---- memory model ---- */

static uint8_t mem_read(UpdiEmu* e, uint32_t addr) {
    if(addr == e->nvmctrl_base + e->status_offset) return 0x00; /* always ready */
    if(addr >= e->flash_base && addr < e->flash_base + EMU_FLASH_SIZE)
        return e->flash[addr - e->flash_base];
    if(addr >= EMU_IO_BASE && addr < EMU_IO_BASE + EMU_IO_SIZE) return e->io[addr - EMU_IO_BASE];
    return 0x00;
}

static void mem_write(UpdiEmu* e, uint32_t addr, uint8_t val) {
    if(addr >= e->flash_base && addr < e->flash_base + EMU_FLASH_SIZE) {
        e->flash[addr - e->flash_base] = val;
        return;
    }
    if(addr == e->nvmctrl_base + e->ctrla_offset) {
        /* NVMCTRL command register. Model a chip erase as flash -> 0xFF. */
        if(e->cmd_chip_erase != 0x00 && val == e->cmd_chip_erase)
            memset(e->flash, 0xFF, EMU_FLASH_SIZE);
        /* Model the P:0 WRITE_FUSE side effect: copy the DATA
         * register to the address held in the ADDR register so read_fuse can verify it. */
        if(e->cmd_write_fuse != 0xFF && val == e->cmd_write_fuse) {
            uint32_t fa = (uint32_t)e->io[e->fuse_addr_offset] |
                          ((uint32_t)e->io[e->fuse_addr_offset + 1] << 8);
            uint8_t fd = e->io[e->fuse_data_offset];
            if(fa >= EMU_IO_BASE && fa < EMU_IO_BASE + EMU_IO_SIZE) e->io[fa - EMU_IO_BASE] = fd;
        }
        if(addr >= EMU_IO_BASE && addr < EMU_IO_BASE + EMU_IO_SIZE) e->io[addr - EMU_IO_BASE] = val;
        return;
    }
    if(addr >= EMU_IO_BASE && addr < EMU_IO_BASE + EMU_IO_SIZE) e->io[addr - EMU_IO_BASE] = val;
}

/* ---- STCS side effects (reset / progmode model) ---- */

static void apply_stcs(UpdiEmu* e, uint8_t addr, uint8_t val) {
    e->cs[addr & 0x0F] = val;
    if(addr == UPDI_ASI_RESET_REQ) {
        if(val == 0x00) {
            /* Reset released: if a key was armed, enter the corresponding mode. */
            if(e->nvm_key) {
                e->cs[UPDI_ASI_SYS_STATUS] |= (1 << UPDI_ASI_SYS_STATUS_NVMPROG);
                e->cs[UPDI_ASI_SYS_STATUS] &= ~(1 << UPDI_ASI_SYS_STATUS_LOCKSTATUS);
            }
            if(e->erase_key) {
                e->cs[UPDI_ASI_SYS_STATUS] &= ~(1 << UPDI_ASI_SYS_STATUS_LOCKSTATUS);
            }
        }
    }
}

/* ---- instruction decoder (one TX byte at a time) ---- */

static void feed_byte(UpdiEmu* e, uint8_t b) {
    switch(e->state) {
    case S_IDLE:
        if(b == UPDI_PHY_SYNC) e->state = S_OPCODE;
        /* else: BREAK (0x00) or stray idle byte — ignore */
        break;

    case S_OPCODE: {
        uint8_t op = b;
        uint8_t grp = op >> 5;
        switch(grp) {
        case 0x4: /* LDCS */
            rx_push(e, e->cs[op & 0x0F]);
            e->state = S_IDLE;
            break;
        case 0x6: /* STCS */
            e->pending_cs_addr = op & 0x0F;
            e->state = S_STCS_VAL;
            break;
        case 0x0: /* LDS */
            e->data_size = (op & 0x01) ? 2 : 1;
            e->need = (op & UPDI_ADDRESS_24) ? 3 : 2;
            e->acclen = 0;
            e->state = S_LDS_ADDR;
            break;
        case 0x2: /* STS */
            e->data_size = (op & 0x01) ? 2 : 1;
            e->need = (op & UPDI_ADDRESS_24) ? 3 : 2;
            e->acclen = 0;
            e->state = S_STS_ADDR;
            break;
        case 0x3: { /* ST */
            uint8_t ptrmode = op & 0x0C;
            if(ptrmode == UPDI_PTR_ADDRESS) {
                e->need = (op & 0x02) ? 3 : 2; /* DATA_24 -> 3, DATA_16 -> 2 */
                e->acclen = 0;
                e->state = S_STPTR_ADDR;
            } else if(ptrmode == UPDI_PTR_INC) {
                int total = e->repeat_next + 1;
                e->repeat_next = 0;
                if((op & 0x03) == UPDI_DATA_16) {
                    e->inc_left = total * 2; /* ACK-less block, raw bytes to consume */
                    e->state = S_STINC16;
                } else {
                    e->inc_left = total; /* ACKed byte block */
                    e->state = S_STINC8;
                }
            } else {
                e->state = S_IDLE;
            }
            break;
        }
        case 0x1: { /* LD */
            uint8_t ptrmode = op & 0x0C;
            if(ptrmode == UPDI_PTR_INC) {
                int total = e->repeat_next + 1;
                e->repeat_next = 0;
                int per = ((op & 0x03) == UPDI_DATA_16) ? 2 : 1;
                for(int i = 0; i < total * per; i++) {
                    rx_push(e, mem_read(e, e->ptr));
                    e->ptr++;
                }
            }
            e->state = S_IDLE;
            break;
        }
        case 0x5: /* REPEAT */
            e->state = S_REPEAT_VAL;
            break;
        case 0x7: { /* KEY */
            int size_idx = op & 0x03;
            if(op & UPDI_KEY_SIB) {
                int n = (size_idx == 2) ? 32 : (size_idx == 1) ? 16 : 8;
                for(int i = 0; i < n; i++) rx_push(e, (i < e->sib_len) ? e->sib[i] : 0x00);
                e->state = S_IDLE;
            } else {
                e->need = 8 << size_idx;
                e->acclen = 0;
                e->state = S_KEY;
            }
            break;
        }
        default:
            e->state = S_IDLE;
            break;
        }
        break;
    }

    case S_STCS_VAL:
        apply_stcs(e, e->pending_cs_addr, b);
        e->state = S_IDLE;
        break;

    case S_REPEAT_VAL:
        e->repeat_next = b; /* counter value = count-1 */
        e->state = S_IDLE;
        break;

    case S_LDS_ADDR:
        e->acc[e->acclen++] = b;
        if(e->acclen >= e->need) {
            e->cur_addr = 0;
            for(int i = 0; i < e->acclen; i++) e->cur_addr |= (uint32_t)e->acc[i] << (8 * i);
            for(int i = 0; i < e->data_size; i++) rx_push(e, mem_read(e, e->cur_addr + i));
            e->state = S_IDLE;
        }
        break;

    case S_STS_ADDR:
        e->acc[e->acclen++] = b;
        if(e->acclen >= e->need) {
            e->cur_addr = 0;
            for(int i = 0; i < e->acclen; i++) e->cur_addr |= (uint32_t)e->acc[i] << (8 * i);
            rx_push(e, UPDI_PHY_ACK); /* ACK the address phase */
            e->acclen = 0;
            e->need = e->data_size;
            e->state = S_STS_DATA;
        }
        break;

    case S_STS_DATA:
        e->acc[e->acclen++] = b;
        if(e->acclen >= e->need) {
            for(int i = 0; i < e->acclen; i++) mem_write(e, e->cur_addr + i, e->acc[i]);
            rx_push(e, UPDI_PHY_ACK); /* ACK the data phase */
            e->state = S_IDLE;
        }
        break;

    case S_STPTR_ADDR:
        e->acc[e->acclen++] = b;
        if(e->acclen >= e->need) {
            e->ptr = 0;
            for(int i = 0; i < e->acclen; i++) e->ptr |= (uint32_t)e->acc[i] << (8 * i);
            rx_push(e, UPDI_PHY_ACK);
            e->state = S_IDLE;
        }
        break;

    case S_STINC8: /* ACKed byte block: write + ACK per byte */
        mem_write(e, e->ptr++, b);
        rx_push(e, UPDI_PHY_ACK);
        if(--e->inc_left <= 0) e->state = S_IDLE;
        break;

    case S_STINC16: /* ACK-less word block: just consume bytes */
        mem_write(e, e->ptr++, b);
        if(--e->inc_left <= 0) e->state = S_IDLE;
        break;

    case S_KEY:
        e->acc[e->acclen++] = b;
        if(e->acclen >= e->need) {
            /* The key arrives reversed on the wire; un-reverse to compare. */
            char key[33];
            for(int i = 0; i < e->need; i++) key[i] = (char)e->acc[e->need - 1 - i];
            key[e->need] = '\0';
            if(e->need == 8 && memcmp(key, UPDI_KEY_NVM, 8) == 0) {
                e->nvm_key = true;
                e->cs[UPDI_ASI_KEY_STATUS] |= (1 << UPDI_ASI_KEY_STATUS_NVMPROG);
            } else if(e->need == 8 && memcmp(key, UPDI_KEY_CHIPERASE, 8) == 0) {
                e->erase_key = true;
                e->cs[UPDI_ASI_KEY_STATUS] |= (1 << UPDI_ASI_KEY_STATUS_CHIPERASE);
            }
            e->state = S_IDLE;
        }
        break;

    default:
        e->state = S_IDLE;
        break;
    }
}

/* ---- transport vtable ---- */

static bool emu_send(void* ctx, const uint8_t* data, size_t len) {
    UpdiEmu* e = (UpdiEmu*)ctx;
    for(size_t i = 0; i < len; i++) {
        if(e->tx_len < EMU_TX_CAP) e->tx[e->tx_len++] = data[i];
        feed_byte(e, data[i]);
    }
    return true;
}

static size_t emu_recv(void* ctx, uint8_t* data, size_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    UpdiEmu* e = (UpdiEmu*)ctx;
    size_t n = 0;
    while(n < len) {
        uint8_t b;
        if(!rx_pop(e, &b)) break;
        data[n++] = b;
    }
    return n;
}

static void emu_double_break(void* ctx) {
    UpdiEmu* e = (UpdiEmu*)ctx;
    e->state = S_IDLE;
}

static uint32_t emu_millis(void* ctx) {
    UpdiEmu* e = (UpdiEmu*)ctx;
    return ++e->now_ms; /* advances every call so poll loops can time out */
}

/* ---- setup ---- */

void updi_emu_init(UpdiEmu* e, UpdiTransport* t) {
    memset(e, 0, sizeof(*e));
    memset(e->flash, 0xFF, EMU_FLASH_SIZE); /* erased flash reads 0xFF */
    e->state = S_IDLE;
    e->cs[UPDI_CS_STATUSA] = 0x30; /* non-zero so updi_link_activate() succeeds */
    e->cmd_write_fuse = 0xFF;
    t->ctx = e;
    t->send = emu_send;
    t->recv = emu_recv;
    t->double_break = emu_double_break;
    t->millis = emu_millis;
}

void updi_emu_clear_tx(UpdiEmu* e) {
    e->tx_len = 0;
}

static void set_sib(UpdiEmu* e, const char* family, char nvm_digit) {
    memset(e->sib, 0, sizeof(e->sib));
    size_t fl = strlen(family);
    if(fl > 7) fl = 7;
    memcpy(e->sib, family, fl);
    e->sib[7] = ' ';
    e->sib[8] = 'P';
    e->sib[9] = ':';
    e->sib[10] = (uint8_t)nvm_digit;
    e->sib[11] = 'D';
    e->sib[12] = ':';
    e->sib[13] = '0';
    e->sib_len = 16;
}

void updi_emu_config_p0(UpdiEmu* e, const uint8_t sig[3], const char* family) {
    e->flash_base = 0x8000;
    e->nvmctrl_base = 0x1000;
    e->status_offset = 0x02;
    e->ctrla_offset = 0x00;
    e->fuse_data_offset = 0x06;
    e->fuse_addr_offset = 0x08;
    e->cmd_write_fuse = 0x07;
    e->cmd_chip_erase = 0x05;
    e->sigrow_addr = 0x1100;
    e->address_bytes = 2;
    memcpy(e->sig, sig, 3);
    /* place signature into the I/O model at the sigrow */
    e->io[0x1100 - EMU_IO_BASE + 0] = sig[0];
    e->io[0x1100 - EMU_IO_BASE + 1] = sig[1];
    e->io[0x1100 - EMU_IO_BASE + 2] = sig[2];
    set_sib(e, family, '0');
}

void updi_emu_config_p3(UpdiEmu* e, const uint8_t sig[3], const char* family) {
    e->flash_base = 0x800000;
    e->nvmctrl_base = 0x1000;
    e->status_offset = 0x06;
    e->ctrla_offset = 0x00;
    e->fuse_data_offset = 0x08;
    e->fuse_addr_offset = 0x0C;
    e->cmd_write_fuse = 0xFF; /* P:3 fuses go via the EEPROM path; no DATA->fuse shortcut */
    e->cmd_chip_erase = 0x20;
    e->sigrow_addr = 0x1100;
    e->address_bytes = 3;
    memcpy(e->sig, sig, 3);
    e->io[0x1100 - EMU_IO_BASE + 0] = sig[0];
    e->io[0x1100 - EMU_IO_BASE + 1] = sig[1];
    e->io[0x1100 - EMU_IO_BASE + 2] = sig[2];
    set_sib(e, family, '3');
}
