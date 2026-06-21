/**
 * @file updi_constants.h
 * @brief UPDI protocol constants.
 *
 * Ported verbatim from pymcuprog/serialupdi/constants.py (the canonical, maintained
 * reference). Do not edit values from memory — cross-check against the Microchip
 * datasheet UPDI/ASI register maps if anything looks off.
 *
 * This header has no Flipper/FURI dependencies so the protocol layer stays portable
 * and host-testable.
 */
#pragma once

#include <stdint.h>

/* UPDI instruction opcodes (high nibble of the instruction byte) */
#define UPDI_BREAK 0x00

#define UPDI_LDS   0x00
#define UPDI_STS   0x40
#define UPDI_LD    0x20
#define UPDI_ST    0x60
#define UPDI_LDCS  0x80
#define UPDI_STCS  0xC0
#define UPDI_REPEAT 0xA0
#define UPDI_KEY   0xE0

/* Pointer access modes (for LD/ST) */
#define UPDI_PTR         0x00
#define UPDI_PTR_INC     0x04
#define UPDI_PTR_ADDRESS 0x08

/* Address size encodings (for LDS/STS) */
#define UPDI_ADDRESS_8  0x00
#define UPDI_ADDRESS_16 0x04
#define UPDI_ADDRESS_24 0x08

/* Data size encodings */
#define UPDI_DATA_8  0x00
#define UPDI_DATA_16 0x01
#define UPDI_DATA_24 0x02

/* KEY instruction sub-modes */
#define UPDI_KEY_SIB 0x04
#define UPDI_KEY_KEY 0x00

/* KEY / SIB size selectors */
#define UPDI_KEY_64  0x00
#define UPDI_KEY_128 0x01
#define UPDI_KEY_256 0x02

#define UPDI_SIB_8BYTES  UPDI_KEY_64
#define UPDI_SIB_16BYTES UPDI_KEY_128
#define UPDI_SIB_32BYTES UPDI_KEY_256

#define UPDI_REPEAT_BYTE 0x00
#define UPDI_REPEAT_WORD 0x01

/* Physical layer constants */
#define UPDI_PHY_SYNC 0x55 /* SYNCH byte — prepended to every instruction frame for autobaud */
#define UPDI_PHY_ACK  0x40 /* ACK returned by the target on ACKed transfers */

/* REPEAT counter is 1 byte with off-by-one counting (max 256 repeats) */
#define UPDI_MAX_REPEAT_SIZE (0xFF + 1)

/* Control/Status (CS) and ASI register address map (used with LDCS/STCS) */
#define UPDI_CS_STATUSA      0x00
#define UPDI_CS_STATUSB      0x01
#define UPDI_CS_CTRLA        0x02
#define UPDI_CS_CTRLB        0x03
#define UPDI_ASI_KEY_STATUS  0x07
#define UPDI_ASI_RESET_REQ   0x08
#define UPDI_ASI_CTRLA       0x09
#define UPDI_ASI_SYS_CTRLA   0x0A
#define UPDI_ASI_SYS_STATUS  0x0B
#define UPDI_ASI_CRC_STATUS  0x0C

/* CTRLA bits */
#define UPDI_CTRLA_IBDLY_BIT 7 /* inter-byte delay enable */
#define UPDI_CTRLA_RSD_BIT   3 /* response-signature disable (ACK-less block writes) */

/* CTRLB bits */
#define UPDI_CTRLB_CCDETDIS_BIT 3 /* collision detection disable */
#define UPDI_CTRLB_UPDIDIS_BIT  2 /* UPDI disable */

/*
 * Programming keys. These are sent LSB-first / reversed on the wire — see
 * updi_link_key(): the byte order is reversed before transmission. Keep the
 * literal strings exactly as Microchip defines them (including the trailing
 * space in "NVMProg ").
 */
#define UPDI_KEY_NVM       "NVMProg "
#define UPDI_KEY_CHIPERASE "NVMErase"
#define UPDI_KEY_UROW      "NVMUs&te"
#define UPDI_KEY_LEN       8

/* STATUSA / STATUSB field positions */
#define UPDI_ASI_STATUSA_REVID 4
#define UPDI_ASI_STATUSB_PESIG 0

/* ASI_KEY_STATUS bits */
#define UPDI_ASI_KEY_STATUS_CHIPERASE 3
#define UPDI_ASI_KEY_STATUS_NVMPROG   4
#define UPDI_ASI_KEY_STATUS_UROWWRITE 5

/* ASI_SYS_STATUS bits */
#define UPDI_ASI_SYS_STATUS_RSTSYS     5
#define UPDI_ASI_SYS_STATUS_INSLEEP    4
#define UPDI_ASI_SYS_STATUS_NVMPROG    3
#define UPDI_ASI_SYS_STATUS_UROWPROG   2
#define UPDI_ASI_SYS_STATUS_LOCKSTATUS 0

/* ASI_SYS_CTRLA bits */
#define UPDI_ASI_SYS_CTRLA_UROW_FINAL 1

/* Value written to ASI_RESET_REQ to apply reset */
#define UPDI_RESET_REQ_VALUE 0x59
