/**
 * @file test_updi.c
 * @brief Host unit tests for the portable UPDI core (no FURI). Built with gcc, run on PC.
 *
 * Exercises: Intel HEX round-trip, device-table lookup, link framing (SYNCH/KEY), and a
 * full end-to-end programming choreography (activate -> SIB -> progmode -> read signature
 * -> chip erase -> write flash page -> read-back verify -> fuse write/read) against the
 * UPDI target emulator, for both NVM P:0 (16-bit) and P:3 (24-bit) variants.
 */
#include <stdio.h>
#include <string.h>

#include "../updi/updi_link.h"
#include "../updi/updi_nvm.h"
#include "../updi/updi_device.h"
#include "../updi/updi_constants.h"
#include "../updi/updi_session.h"
#include "../hex/intel_hex.h"
#include "updi_emu.h"

static int g_checks = 0, g_fails = 0;

static bool all_ff_region(const uint8_t* p, size_t n) {
    for(size_t i = 0; i < n; i++)
        if(p[i] != 0xFF) return false;
    return true;
}

#define CHECK(cond, msg)                                  \
    do {                                                  \
        g_checks++;                                       \
        if(!(cond)) {                                     \
            g_fails++;                                    \
            printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                 \
    } while(0)

#define CHECK_EQ(a, b, msg)                                                        \
    do {                                                                           \
        g_checks++;                                                                \
        long _a = (long)(a), _b = (long)(b);                                       \
        if(_a != _b) {                                                             \
            g_fails++;                                                             \
            printf("  FAIL: %s: got %ld expected %ld (%s:%d)\n", msg, _a, _b, __FILE__, __LINE__); \
        }                                                                          \
    } while(0)

/* ---- Intel HEX ---- */

typedef struct {
    char buf[8192];
    size_t len;
} HexSink;

static bool hex_sink(void* ctx, const char* s, size_t len) {
    HexSink* hs = (HexSink*)ctx;
    if(hs->len + len >= sizeof(hs->buf)) return false;
    memcpy(hs->buf + hs->len, s, len);
    hs->len += len;
    return true;
}

static void test_intel_hex(void) {
    printf("test_intel_hex\n");
    /* Build a known image, write it to HEX, parse it back, compare. */
    uint8_t src[300];
    for(size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 7 + 3);

    HexSink hs = {0};
    IntelHexStatus ws = intel_hex_write(src, sizeof(src), 0, 16, hex_sink, &hs);
    CHECK_EQ(ws, IntelHexOk, "hex write ok");

    uint8_t img_buf[512];
    IntelHexImage img;
    intel_hex_image_init(&img, img_buf, sizeof(img_buf), 0, 0xFF);

    /* Feed the produced text line by line. */
    char* text = hs.buf;
    char line[128];
    size_t li = 0;
    for(size_t i = 0; i <= hs.len; i++) {
        char c = (i < hs.len) ? text[i] : '\n';
        if(c == '\n') {
            line[li] = '\0';
            if(li > 0) {
                IntelHexStatus ps = intel_hex_parse_line(&img, line);
                CHECK_EQ(ps, IntelHexOk, "hex parse line ok");
            }
            li = 0;
        } else if(c != '\r' && li < sizeof(line) - 1) {
            line[li++] = c;
        }
    }
    CHECK(img.eof_seen, "hex eof seen");
    CHECK_EQ(img.min_offset, 0, "hex min offset");
    CHECK_EQ(img.max_offset, sizeof(src), "hex max offset");
    CHECK_EQ(memcmp(img_buf, src, sizeof(src)), 0, "hex round-trip data matches");

    /* Checksum rejection. */
    IntelHexImage img2;
    intel_hex_image_init(&img2, img_buf, sizeof(img_buf), 0, 0xFF);
    CHECK_EQ(
        intel_hex_parse_line(&img2, ":0400000037363534FF"),
        IntelHexErrChecksum,
        "hex bad checksum rejected");
    /* A valid record (checksum computed): :04 0000 00 37363534 -> sum bytes = 04+37+36+35+34=DA, cksum=26 */
    CHECK_EQ(
        intel_hex_parse_line(&img2, ":040000003736353426"),
        IntelHexOk,
        "hex valid record accepted");
    CHECK_EQ(img2.data[0], 0x37, "hex parsed byte 0");
    CHECK_EQ(img2.data[3], 0x34, "hex parsed byte 3");

    /* Extended linear address handling. */
    IntelHexImage img3;
    static uint8_t big[0x20000];
    intel_hex_image_init(&img3, big, sizeof(big), 0, 0xFF);
    CHECK_EQ(intel_hex_parse_line(&img3, ":020000040001F9"), IntelHexOk, "ext linear set");
    /* :01 0000 00 AA -> cksum: 01+00+00+00+AA=AB -> 0x55 */
    CHECK_EQ(intel_hex_parse_line(&img3, ":01000000AA55"), IntelHexOk, "data at 0x10000");
    CHECK_EQ(big[0x10000], 0xAA, "ext linear data placed at 0x10000");
}

/* ---- device table ---- */

static void test_device_table(void) {
    printf("test_device_table\n");
    const UpdiDevice* d = updi_device_find_by_id(0x1E961E);
    CHECK(d != NULL, "avr64ea48 found");
    if(d) {
        CHECK_EQ(strcmp(d->name, "avr64ea48"), 0, "avr64ea48 name");
        CHECK_EQ(d->flash_address, 0x800000, "avr64ea48 flash base");
        CHECK_EQ(d->flash_page_size, 0x80, "avr64ea48 page size");
        CHECK_EQ(d->address_bytes, 3, "avr64ea48 24-bit");
        CHECK_EQ(d->nvm_variant, UpdiNvmVariantP3, "avr64ea48 P3");
    }
    const UpdiDevice* t = updi_device_find_by_id(0x1E9422);
    CHECK(t != NULL, "attiny1614 found");
    if(t) {
        CHECK_EQ(t->address_bytes, 2, "attiny1614 16-bit");
        CHECK_EQ(t->nvm_variant, UpdiNvmVariantP0, "attiny1614 P0");
    }
    CHECK(updi_device_find_by_id(0x123456) == NULL, "unknown id rejected");
}

/* ---- link framing ---- */

static void test_framing(void) {
    printf("test_framing\n");
    UpdiEmu emu;
    UpdiTransport t;
    updi_emu_init(&emu, &t);
    uint8_t sig[3] = {0x1E, 0x94, 0x22};
    updi_emu_config_p0(&emu, sig, "tinyAVR");
    UpdiLink link;
    updi_link_init(&link, &t);

    uint8_t v = 0;
    updi_emu_clear_tx(&emu);
    CHECK_EQ(updi_link_ldcs(&link, UPDI_CS_STATUSA, &v), UpdiOk, "ldcs ok");
    CHECK_EQ(emu.tx_len, 2, "ldcs tx len");
    CHECK_EQ(emu.tx[0], 0x55, "ldcs sync");
    CHECK_EQ(emu.tx[1], 0x80, "ldcs opcode STATUSA");

    updi_emu_clear_tx(&emu);
    CHECK_EQ(updi_link_stcs(&link, UPDI_CS_CTRLB, 0xAB), UpdiOk, "stcs ok");
    CHECK_EQ(emu.tx_len, 3, "stcs tx len");
    CHECK_EQ(emu.tx[0], 0x55, "stcs sync");
    CHECK_EQ(emu.tx[1], 0xC0 | UPDI_CS_CTRLB, "stcs opcode");
    CHECK_EQ(emu.tx[2], 0xAB, "stcs value");

    /* KEY must be transmitted reversed. */
    updi_emu_clear_tx(&emu);
    CHECK_EQ(updi_link_key(&link, UPDI_KEY_64, UPDI_KEY_NVM, 8), UpdiOk, "key ok");
    CHECK_EQ(emu.tx_len, 2 + 8, "key tx len");
    CHECK_EQ(emu.tx[0], 0x55, "key sync");
    CHECK_EQ(emu.tx[1], 0xE0 | UPDI_KEY_64, "key opcode");
    const char* nvm = UPDI_KEY_NVM; /* "NVMProg " */
    for(int i = 0; i < 8; i++)
        CHECK_EQ(emu.tx[2 + i], (uint8_t)nvm[7 - i], "key byte reversed");
}

/* ---- end-to-end programming, parameterised by variant ---- */

static void run_program_flow(bool p3, const uint8_t sig[3], const char* family, uint32_t expect_id) {
    UpdiEmu emu;
    UpdiTransport t;
    updi_emu_init(&emu, &t);
    if(p3)
        updi_emu_config_p3(&emu, sig, family);
    else
        updi_emu_config_p0(&emu, sig, family);

    UpdiLink link;
    updi_link_init(&link, &t);

    CHECK_EQ(updi_link_activate(&link), UpdiOk, "activate");

    UpdiSibInfo info;
    CHECK_EQ(updi_nvm_read_sib_info(&link, &info), UpdiOk, "read sib");
    CHECK_EQ(info.nvm_version, p3 ? 3 : 0, "sib nvm version");
    CHECK_EQ(strcmp(info.family, family), 0, "sib family");

    /* Choose address size from the SIB (16-bit for P:0, 24-bit otherwise). */
    updi_link_set_address_size(&link, (info.nvm_version == 0) ? 2 : 3);

    UpdiNvm nvm;
    updi_nvm_init(&nvm, &link, NULL);
    CHECK_EQ(updi_nvm_enter_progmode(&nvm), UpdiOk, "enter progmode");
    CHECK(updi_nvm_in_progmode(&nvm), "in progmode");
    CHECK(!updi_nvm_is_locked(&nvm), "not locked");

    uint8_t rsig[3] = {0};
    CHECK_EQ(updi_nvm_read_signature(&nvm, rsig), UpdiOk, "read signature");
    uint32_t id = updi_device_id_from_sig(rsig);
    CHECK_EQ(id, expect_id, "signature id matches");

    const UpdiDevice* dev = updi_device_find_by_id(id);
    CHECK(dev != NULL, "device identified");
    if(!dev) return;
    updi_nvm_set_device(&nvm, dev);

    CHECK_EQ(updi_nvm_chip_erase(&nvm), UpdiOk, "chip erase");

    /* Write a full flash page with a known pattern, then read it back and compare. */
    size_t page = dev->flash_page_size;
    uint8_t page_data[256];
    for(size_t i = 0; i < page; i++) page_data[i] = (uint8_t)(0xA0 ^ (i * 3 + 1));
    CHECK_EQ(
        updi_nvm_write_flash_page(&nvm, dev->flash_address, page_data, page),
        UpdiOk,
        "write flash page");

    uint8_t readback[256];
    memset(readback, 0, sizeof(readback));
    CHECK_EQ(updi_nvm_read(&nvm, dev->flash_address, readback, page), UpdiOk, "read flash back");
    CHECK_EQ(memcmp(page_data, readback, page), 0, "flash page round-trip matches");

    /* Write a second page at the next page boundary to exercise addressing. */
    uint32_t addr2 = dev->flash_address + page;
    uint8_t page2[256];
    for(size_t i = 0; i < page; i++) page2[i] = (uint8_t)(i + 0x10);
    CHECK_EQ(updi_nvm_write_flash_page(&nvm, addr2, page2, page), UpdiOk, "write 2nd page");
    CHECK_EQ(updi_nvm_read(&nvm, addr2, readback, page), UpdiOk, "read 2nd page");
    CHECK_EQ(memcmp(page2, readback, page), 0, "2nd page matches");

    /* Fuse write + read-back. */
    CHECK_EQ(updi_nvm_write_fuse(&nvm, 2, 0x5A), UpdiOk, "write fuse");
    uint8_t fuse = 0;
    CHECK_EQ(updi_nvm_read_fuse(&nvm, 2, &fuse), UpdiOk, "read fuse");
    CHECK_EQ(fuse, 0x5A, "fuse round-trip");

    CHECK_EQ(updi_nvm_leave_progmode(&nvm), UpdiOk, "leave progmode");
}

static void test_program_p0(void) {
    printf("test_program_p0 (attiny1614)\n");
    uint8_t sig[3] = {0x1E, 0x94, 0x22};
    run_program_flow(false, sig, "tinyAVR", 0x1E9422);
}

static void test_program_p3(void) {
    printf("test_program_p3 (avr64ea48)\n");
    uint8_t sig[3] = {0x1E, 0x96, 0x1E};
    run_program_flow(true, sig, "AVR EA", 0x1E961E);
}

/* ---- session layer: image write (with all-0xFF page skip) + verify + dump ---- */

static size_t g_prog_last;
static void prog_cb(void* ctx, size_t done, size_t total) {
    (void)ctx;
    (void)total;
    g_prog_last = done;
}

static void test_session_p3(void) {
    printf("test_session_p3 (avr64ea48 image write/verify/dump)\n");
    UpdiEmu emu;
    UpdiTransport t;
    updi_emu_init(&emu, &t);
    uint8_t sig[3] = {0x1E, 0x96, 0x1E};
    updi_emu_config_p3(&emu, sig, "AVR EA");

    UpdiSession s;
    updi_session_init(&s, &t);
    CHECK_EQ(updi_session_connect(&s), UpdiOk, "session connect");
    const UpdiDevice* dev = updi_session_device(&s);
    CHECK(dev && strcmp(dev->name, "avr64ea48") == 0, "session identified avr64ea48");
    if(!dev) return;

    size_t page = dev->flash_page_size; /* 128 */
    size_t len = page * 3;
    uint8_t img[3 * 128];
    memset(img, 0xFF, sizeof(img)); /* page 1 stays all-0xFF -> must be skipped */
    for(size_t i = 0; i < page; i++) {
        img[i] = (uint8_t)(i + 1); /* page 0 */
        img[2 * page + i] = (uint8_t)(0xC0 ^ i); /* page 2 */
    }

    CHECK_EQ(updi_session_chip_erase(&s), UpdiOk, "session chip erase");
    CHECK_EQ(updi_session_write_flash(&s, img, len, prog_cb, NULL), UpdiOk, "session write image");
    CHECK_EQ(g_prog_last, len, "progress reached total");
    CHECK_EQ(
        updi_session_verify_flash(&s, img, len, NULL, NULL, NULL), UpdiOk, "session verify image");

    uint8_t dump[3 * 128];
    CHECK_EQ(updi_session_read_flash(&s, dump, len, NULL, NULL), UpdiOk, "session read flash");
    CHECK_EQ(memcmp(dump, img, len), 0, "dump matches image");
    /* page 1 must read back as erased 0xFF (it was skipped) */
    CHECK(all_ff_region(dump + page, page), "skipped page is 0xFF");

    /* A verify against wrong data must fail. */
    uint8_t bad[3 * 128];
    memcpy(bad, img, len);
    bad[5] ^= 0xFF;
    CHECK_EQ(
        updi_session_verify_flash(&s, bad, len, NULL, NULL, NULL),
        UpdiErrVerify,
        "verify catches mismatch");

    /* The mismatch report identifies the first differing byte (image vs device). */
    UpdiVerifyMismatch mm = {.offset = 9999, .expected = 0x11, .actual = 0x22};
    CHECK_EQ(
        updi_session_verify_flash(&s, bad, len, NULL, NULL, &mm),
        UpdiErrVerify,
        "verify reports mismatch");
    CHECK_EQ(mm.offset, 5, "mismatch offset is first differing byte");
    CHECK_EQ(mm.expected, bad[5], "mismatch expected = supplied image byte");
    CHECK_EQ(mm.actual, img[5], "mismatch actual = device byte");

    updi_session_disconnect(&s);
}

int main(void) {
    test_intel_hex();
    test_device_table();
    test_framing();
    test_program_p0();
    test_program_p3();
    test_session_p3();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
