/**
 * @file updi_transport.h
 * @brief Thin transport seam between the portable UPDI protocol layers and the hardware.
 *
 * The entire updi/ stack talks UPDI *only* through this interface. It never touches
 * FURI/STM32 directly. This keeps the protocol auditable against pymcuprog and lets
 * the host unit tests substitute a mock transport (see host_test/).
 *
 * Wiring model (resistor bridge, v1): Flipper USART TX (pin 13) and RX (pin 14) are
 * tied to a single UPDI line, so every byte we transmit is echoed back to us on RX.
 * The transport's send() implementation MUST transmit the bytes and then read and
 * discard exactly that many echoed bytes, leaving only genuine target responses for
 * recv(). This mirrors pymcuprog's UpdiPhysical.send(), which writes then reads back
 * len(command) bytes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct UpdiTransport UpdiTransport;

struct UpdiTransport {
    /** Implementation-private context pointer (e.g. the furi transport object). */
    void* ctx;

    /**
     * Transmit @p len bytes and discard the @p len echoed bytes (bridge wiring).
     * @return true if all bytes were sent and their echo consumed, false otherwise.
     */
    bool (*send)(void* ctx, const uint8_t* data, size_t len);

    /**
     * Receive up to @p len genuine (non-echo) bytes from the target, waiting at most
     * @p timeout_ms in total.
     * @return number of bytes actually received (may be < len on timeout).
     */
    size_t (*recv)(void* ctx, uint8_t* data, size_t len, uint32_t timeout_ms);

    /**
     * Force the UPDI state machine to a known state by sending a double BREAK.
     * A BREAK is a slow (low) frame; pymcuprog drops to 300 baud so the low pulse is
     * ~30 ms, then restores the working baud/framing. Optional but used by
     * updi_link_activate() for recovery; may be NULL if unsupported.
     */
    void (*double_break)(void* ctx);

    /**
     * Free-running millisecond counter, used by the NVM layer for status-poll timeouts
     * (the equivalent of pymcuprog's Timeout class). Wraparound-safe via unsigned diff.
     * On Flipper this wraps furi_get_tick(); the host test supplies a fake counter.
     */
    uint32_t (*millis)(void* ctx);
};
