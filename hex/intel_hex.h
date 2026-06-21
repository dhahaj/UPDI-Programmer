/**
 * @file intel_hex.h
 * @brief Minimal Intel HEX parser + writer for SD-card flash images and dumps.
 *
 * Parser: feed it lines one at a time; it scatters data records into a caller-provided
 * image buffer (pre-filled with 0xFF) and tracks the written address range. Handles
 * record types 00 (data), 01 (EOF), 02 (extended segment), 04 (extended linear); ignores
 * 03/05 (start address). Validates the per-record checksum.
 *
 * Writer: streams data + extended-linear-address + EOF records to a sink callback, so a
 * flash dump can be written straight to a File without building one huge string.
 *
 * No FURI dependencies — host-testable.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    IntelHexOk = 0,
    IntelHexErrFormat, /* not a valid ':' record / bad hex digits / short line */
    IntelHexErrChecksum, /* record checksum mismatch */
    IntelHexErrOverflow, /* data address falls outside the image buffer */
    IntelHexErrUnsupported, /* unsupported record type */
} IntelHexStatus;

typedef struct {
    uint8_t* data; /* caller-provided image buffer */
    size_t capacity; /* size of @data */
    uint32_t base_offset; /* subtracted from absolute HEX address to get a buffer offset */
    uint32_t ext_base; /* current extended (segment/linear) base address */
    size_t min_offset; /* lowest written offset (valid only if any==true) */
    size_t max_offset; /* highest written offset + 1 */
    bool any; /* any data record stored */
    bool eof_seen; /* a type-01 record was parsed */
} IntelHexImage;

/**
 * Initialise an image over @p buf of @p capacity bytes, filling it with @p fill
 * (use 0xFF for flash). @p base_offset is subtracted from absolute HEX addresses (use 0
 * for a typical flash image whose HEX addresses start at 0).
 */
void intel_hex_image_init(
    IntelHexImage* img,
    uint8_t* buf,
    size_t capacity,
    uint32_t base_offset,
    uint8_t fill);

/** Parse one HEX line (trailing CR/LF and whitespace tolerated). */
IntelHexStatus intel_hex_parse_line(IntelHexImage* img, const char* line);

/** Sink used by the writer; receives NUL-terminated chunks (len excludes the NUL). */
typedef bool (*IntelHexWriteFn)(void* ctx, const char* str, size_t len);

/**
 * Write @p len bytes from @p data as Intel HEX, addresses starting at @p start_address,
 * @p bytes_per_line data bytes per record (typ. 16 or 32). Emits type-04 records at 64K
 * boundaries and a final EOF record. Returns IntelHexOk on success, IntelHexErrFormat if
 * the sink reports a write failure.
 */
IntelHexStatus intel_hex_write(
    const uint8_t* data,
    size_t len,
    uint32_t start_address,
    size_t bytes_per_line,
    IntelHexWriteFn sink,
    void* ctx);
