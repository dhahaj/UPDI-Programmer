/**
 * @file transport_furi.h
 * @brief Flipper implementation of updi_transport.h over furi_hal_serial (USART, pins 13/14).
 *
 * Handles the resistor-bridge echo (TX loops back on RX), 8E2 framing, the 300-baud
 * double BREAK, and a millisecond clock. This is the ONLY place in the app that touches
 * the serial HAL; the whole updi/ stack stays hardware-agnostic.
 */
#pragma once

#include "../updi/updi_transport.h"

typedef struct TransportFuri TransportFuri;

/** Acquire the USART, init at @p baud with 8E2 framing, start async RX. NULL on failure. */
TransportFuri* transport_furi_alloc(uint32_t baud);

/** Stop RX, release the USART and free. */
void transport_furi_free(TransportFuri* tr);

/** The transport vtable to hand to updi_link_init(). */
const UpdiTransport* transport_furi_iface(TransportFuri* tr);

/** Change baud at runtime (framing stays 8E2). */
void transport_furi_set_baud(TransportFuri* tr, uint32_t baud);

/** True if the USART handle was acquired successfully. */
bool transport_furi_ready(TransportFuri* tr);
