/**
 * @file transport_furi.c
 * @brief furi_hal_serial transport for UPDI. See transport_furi.h.
 *
 * API verified against the Momentum (mntm-012) SDK headers:
 *   furi_hal_serial.h, furi_hal_serial_control.h, furi_hal_serial_types.h.
 * The furi_hal_serial RX path is interrupt/callback-driven, so the ISR callback pushes
 * bytes into a FuriStreamBuffer that the synchronous recv() drains. Do not trust these
 * signatures from memory — they have drifted across firmware versions.
 */
#include "transport_furi.h"

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>

#define UPDI_RX_STREAM_SIZE 512u
#define UPDI_ECHO_TIMEOUT_MS 120u /* echo loops back almost immediately */

struct TransportFuri {
    UpdiTransport iface;
    FuriHalSerialHandle* handle;
    FuriStreamBuffer* rx_stream;
    uint32_t baud;
};

/* Apply our fixed UPDI framing: 8 data bits, even parity, 2 stop bits (8E2). */
static void apply_framing_8e2(FuriHalSerialHandle* handle) {
    furi_hal_serial_configure_framing(
        handle, FuriHalSerialDataBits8, FuriHalSerialParityEven, FuriHalSerialStopBits2);
}

/* RX ISR callback: drain the HW FIFO into the stream buffer. Runs in interrupt context. */
static void rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    TransportFuri* tr = context;
    if(event & FuriHalSerialRxEventData) {
        while(furi_hal_serial_async_rx_available(handle)) {
            uint8_t data = furi_hal_serial_async_rx(handle);
            furi_stream_buffer_send(tr->rx_stream, &data, 1, 0);
        }
    }
}

/* ---- transport vtable implementations ---- */

static size_t furi_recv_impl(void* ctx, uint8_t* data, size_t len, uint32_t timeout_ms) {
    TransportFuri* tr = ctx;
    size_t got = 0;
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(timeout_ms);
    while(got < len) {
        uint32_t now = furi_get_tick();
        if(now >= deadline) {
            /* final non-blocking drain */
            got += furi_stream_buffer_receive(tr->rx_stream, data + got, len - got, 0);
            break;
        }
        size_t n = furi_stream_buffer_receive(tr->rx_stream, data + got, len - got, deadline - now);
        if(n == 0) break; /* waited the remaining time, nothing arrived => timeout */
        got += n;
    }
    return got;
}

static bool furi_send_impl(void* ctx, const uint8_t* data, size_t len) {
    TransportFuri* tr = ctx;
    furi_hal_serial_tx(tr->handle, data, len);
    furi_hal_serial_tx_wait_complete(tr->handle);

    /* Bridge wiring: every transmitted byte echoes back on RX. Read and drop exactly
     * len echoed bytes so recv() only ever sees genuine target responses. */
    uint8_t scratch[64];
    size_t discarded = 0;
    while(discarded < len) {
        size_t want = len - discarded;
        if(want > sizeof(scratch)) want = sizeof(scratch);
        size_t n = furi_recv_impl(ctx, scratch, want, UPDI_ECHO_TIMEOUT_MS);
        if(n == 0) return false; /* echo never came back -> check wiring / target power */
        discarded += n;
    }
    return true;
}

static void furi_double_break_impl(void* ctx) {
    TransportFuri* tr = ctx;
    /* A BREAK is a 0x00 frame; at 300 baud the low period (~30 ms) is a proper UPDI BREAK
     * that resets the autobaud state machine. Mirrors pymcuprog send_double_break(). */
    furi_hal_serial_set_br(tr->handle, 300);
    furi_hal_serial_configure_framing(
        tr->handle, FuriHalSerialDataBits8, FuriHalSerialParityEven, FuriHalSerialStopBits1);
    furi_stream_buffer_reset(tr->rx_stream);

    uint8_t brk = 0x00;
    furi_hal_serial_tx(tr->handle, &brk, 1);
    furi_hal_serial_tx_wait_complete(tr->handle);
    furi_delay_ms(20);
    furi_hal_serial_tx(tr->handle, &brk, 1);
    furi_hal_serial_tx_wait_complete(tr->handle);
    furi_delay_ms(20);

    /* Restore the working baud and 8E2 framing; the target re-autobauds on the next SYNCH. */
    furi_hal_serial_set_br(tr->handle, tr->baud);
    apply_framing_8e2(tr->handle);
    furi_stream_buffer_reset(tr->rx_stream);
}

static uint32_t furi_millis_impl(void* ctx) {
    (void)ctx;
    uint32_t freq = furi_kernel_get_tick_frequency();
    if(freq == 0) return furi_get_tick();
    return (uint32_t)((uint64_t)furi_get_tick() * 1000u / freq);
}

/* ---- lifecycle ---- */

TransportFuri* transport_furi_alloc(uint32_t baud) {
    TransportFuri* tr = malloc(sizeof(TransportFuri));
    memset(tr, 0, sizeof(*tr));
    tr->baud = baud;
    tr->rx_stream = furi_stream_buffer_alloc(UPDI_RX_STREAM_SIZE, 1);

    tr->handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(tr->handle) {
        furi_hal_serial_init(tr->handle, baud);
        apply_framing_8e2(tr->handle);
        furi_hal_serial_async_rx_start(tr->handle, rx_callback, tr, false);
    }

    tr->iface.ctx = tr;
    tr->iface.send = furi_send_impl;
    tr->iface.recv = furi_recv_impl;
    tr->iface.double_break = furi_double_break_impl;
    tr->iface.millis = furi_millis_impl;
    return tr;
}

void transport_furi_free(TransportFuri* tr) {
    if(!tr) return;
    if(tr->handle) {
        furi_hal_serial_async_rx_stop(tr->handle);
        furi_hal_serial_deinit(tr->handle);
        furi_hal_serial_control_release(tr->handle);
    }
    if(tr->rx_stream) furi_stream_buffer_free(tr->rx_stream);
    free(tr);
}

const UpdiTransport* transport_furi_iface(TransportFuri* tr) {
    return &tr->iface;
}

void transport_furi_set_baud(TransportFuri* tr, uint32_t baud) {
    tr->baud = baud;
    if(tr->handle) furi_hal_serial_set_br(tr->handle, baud);
}

bool transport_furi_ready(TransportFuri* tr) {
    return tr && tr->handle != NULL;
}
