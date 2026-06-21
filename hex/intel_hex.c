/**
 * @file intel_hex.c
 * @brief Intel HEX parser/writer implementation. See intel_hex.h.
 */
#include "intel_hex.h"

#include <string.h>

/* Record types */
#define IHEX_TYPE_DATA 0x00
#define IHEX_TYPE_EOF 0x01
#define IHEX_TYPE_EXT_SEGMENT 0x02
#define IHEX_TYPE_EXT_LINEAR 0x04

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse two hex chars at p into a byte; return false on bad digit. */
static bool hex_byte(const char* p, uint8_t* out) {
    int hi = hex_nibble(p[0]);
    int lo = hex_nibble(p[1]);
    if(hi < 0 || lo < 0) return false;
    *out = (uint8_t)((hi << 4) | lo);
    return true;
}

void intel_hex_image_init(
    IntelHexImage* img,
    uint8_t* buf,
    size_t capacity,
    uint32_t base_offset,
    uint8_t fill) {
    img->data = buf;
    img->capacity = capacity;
    img->base_offset = base_offset;
    img->ext_base = 0;
    img->min_offset = 0;
    img->max_offset = 0;
    img->any = false;
    img->eof_seen = false;
    if(buf && capacity) memset(buf, fill, capacity);
}

IntelHexStatus intel_hex_parse_line(IntelHexImage* img, const char* line) {
    /* Skip leading whitespace; tolerate blank lines. */
    while(*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') line++;
    if(*line == '\0') return IntelHexOk; /* blank line, nothing to do */
    if(*line != ':') return IntelHexErrFormat;
    line++;

    /* Need at least count(2) + addr(4) + type(2) + checksum(2) = 10 hex chars. */
    uint8_t byte_count, addr_hi, addr_lo, type;
    if(!hex_byte(line + 0, &byte_count)) return IntelHexErrFormat;
    if(!hex_byte(line + 2, &addr_hi)) return IntelHexErrFormat;
    if(!hex_byte(line + 4, &addr_lo)) return IntelHexErrFormat;
    if(!hex_byte(line + 6, &type)) return IntelHexErrFormat;

    uint8_t sum = (uint8_t)(byte_count + addr_hi + addr_lo + type);
    uint16_t record_addr = (uint16_t)((addr_hi << 8) | addr_lo);

    const char* dp = line + 8;
    uint8_t data[256];
    for(uint16_t i = 0; i < byte_count; i++) {
        if(!hex_byte(dp + (size_t)i * 2, &data[i])) return IntelHexErrFormat;
        sum = (uint8_t)(sum + data[i]);
    }

    uint8_t checksum;
    if(!hex_byte(dp + (size_t)byte_count * 2, &checksum)) return IntelHexErrFormat;
    /* Two's-complement checksum: all bytes incl. checksum must sum to 0 (mod 256). */
    if((uint8_t)(sum + checksum) != 0) return IntelHexErrChecksum;

    switch(type) {
    case IHEX_TYPE_DATA: {
        uint32_t abs_addr = img->ext_base + record_addr;
        if(abs_addr < img->base_offset) return IntelHexErrOverflow;
        size_t offset = (size_t)(abs_addr - img->base_offset);
        if(offset + byte_count > img->capacity) return IntelHexErrOverflow;
        memcpy(img->data + offset, data, byte_count);
        if(!img->any) {
            img->min_offset = offset;
            img->max_offset = offset + byte_count;
            img->any = true;
        } else {
            if(offset < img->min_offset) img->min_offset = offset;
            if(offset + byte_count > img->max_offset) img->max_offset = offset + byte_count;
        }
        break;
    }
    case IHEX_TYPE_EOF:
        img->eof_seen = true;
        break;
    case IHEX_TYPE_EXT_LINEAR:
        if(byte_count != 2) return IntelHexErrFormat;
        img->ext_base = (uint32_t)((data[0] << 8) | data[1]) << 16;
        break;
    case IHEX_TYPE_EXT_SEGMENT:
        if(byte_count != 2) return IntelHexErrFormat;
        img->ext_base = (uint32_t)((data[0] << 8) | data[1]) << 4;
        break;
    case 0x03: /* start segment address */
    case 0x05: /* start linear address */
        break; /* ignored — not relevant for programming */
    default:
        return IntelHexErrUnsupported;
    }
    return IntelHexOk;
}

/* ---- writer ---- */

static char nib(uint8_t v) {
    v &= 0x0F;
    return (char)(v < 10 ? ('0' + v) : ('A' + v - 10));
}

static char* put_byte(char* p, uint8_t v, uint8_t* sum) {
    *p++ = nib(v >> 4);
    *p++ = nib(v);
    *sum = (uint8_t)(*sum + v);
    return p;
}

static bool emit_record(
    IntelHexWriteFn sink,
    void* ctx,
    uint8_t count,
    uint16_t addr,
    uint8_t type,
    const uint8_t* data) {
    /* Max line: ':' + count + addr(2) + type + 255 data + checksum + CRLF + NUL */
    char buf[1 + 2 + 4 + 2 + 255 * 2 + 2 + 3];
    char* p = buf;
    uint8_t sum = 0;
    *p++ = ':';
    p = put_byte(p, count, &sum);
    p = put_byte(p, (uint8_t)(addr >> 8), &sum);
    p = put_byte(p, (uint8_t)(addr & 0xFF), &sum);
    p = put_byte(p, type, &sum);
    for(uint8_t i = 0; i < count; i++) p = put_byte(p, data[i], &sum);
    p = put_byte(p, (uint8_t)(0x100 - sum), &sum); /* checksum = two's complement */
    *p++ = '\r';
    *p++ = '\n';
    size_t len = (size_t)(p - buf);
    *p = '\0';
    return sink(ctx, buf, len);
}

IntelHexStatus intel_hex_write(
    const uint8_t* data,
    size_t len,
    uint32_t start_address,
    size_t bytes_per_line,
    IntelHexWriteFn sink,
    void* ctx) {
    if(bytes_per_line == 0 || bytes_per_line > 255) bytes_per_line = 16;

    uint16_t cur_upper = 0xFFFF; /* force an initial type-04 record */
    size_t off = 0;
    while(off < len) {
        uint32_t abs_addr = start_address + off;
        uint16_t upper = (uint16_t)(abs_addr >> 16);
        if(upper != cur_upper) {
            uint8_t ext[2] = {(uint8_t)(upper >> 8), (uint8_t)(upper & 0xFF)};
            if(!emit_record(sink, ctx, 2, 0, IHEX_TYPE_EXT_LINEAR, ext))
                return IntelHexErrFormat;
            cur_upper = upper;
        }
        size_t chunk = len - off;
        if(chunk > bytes_per_line) chunk = bytes_per_line;
        /* Do not let a record cross a 64K boundary (address is 16-bit within a record). */
        uint16_t lo = (uint16_t)(abs_addr & 0xFFFF);
        if((uint32_t)lo + chunk > 0x10000UL) chunk = 0x10000UL - lo;
        if(!emit_record(sink, ctx, (uint8_t)chunk, lo, IHEX_TYPE_DATA, data + off))
            return IntelHexErrFormat;
        off += chunk;
    }

    if(!emit_record(sink, ctx, 0, 0, IHEX_TYPE_EOF, NULL)) return IntelHexErrFormat;
    return IntelHexOk;
}
