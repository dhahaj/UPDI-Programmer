/**
 * @file updi_worker.c
 * @brief Background worker thread that runs the long UPDI operations and the SD-card file
 *        I/O (Intel HEX), so the GUI thread stays responsive. Progress is published into
 *        the shared app fields under app->lock; completion is signalled to the progress
 *        scene with a custom ViewDispatcher event.
 */
#include "updi_app.h"
#include "updi_scene.h"

#include <furi.h>

#define WORKER_STACK 4096u
#define FILE_READ_CHUNK 256u
#define HEX_LINE_MAX 300u

const char* updi_status_str(UpdiStatus s) {
    switch(s) {
    case UpdiOk: return "OK";
    case UpdiErrTimeout: return "No response (timeout)";
    case UpdiErrNack: return "Target NACK";
    case UpdiErrProtocol: return "Protocol error";
    case UpdiErrParam: return "Bad parameter";
    case UpdiErrVerify: return "Verify mismatch";
    case UpdiErrNvm: return "NVM write error";
    case UpdiErrLocked: return "Device locked";
    case UpdiErrUnsupported: return "Unknown signature";
    case UpdiErrTransport: return "Serial busy/failed";
    default: return "Error";
    }
}

static void set_phase(UpdiApp* app, UpdiPhase ph, size_t done, size_t total) {
    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->phase = ph;
    app->prog_done = done;
    app->prog_total = total;
    furi_mutex_release(app->lock);
}

static void worker_progress(void* ctx, size_t done, size_t total) {
    UpdiApp* app = ctx;
    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->prog_done = done;
    app->prog_total = total;
    furi_mutex_release(app->lock);
}

/* ---- connection helpers ---- */

static UpdiStatus ensure_transport(UpdiApp* app) {
    if(!app->transport) {
        app->transport = transport_furi_alloc(app->baud);
        if(!transport_furi_ready(app->transport)) return UpdiErrTransport;
        updi_session_init(&app->session, transport_furi_iface(app->transport));
    }
    return UpdiOk;
}

static UpdiStatus do_connect(UpdiApp* app, bool allow_erase) {
    UpdiStatus st = ensure_transport(app);
    if(st != UpdiOk) return st;
    st = updi_session_connect(&app->session);
    if(st == UpdiErrLocked && allow_erase) {
        st = updi_session_unlock(&app->session);
        if(st == UpdiOk) st = updi_session_connect(&app->session);
    }
    if(st == UpdiOk) app->have_device = true;
    return st;
}

/* ---- Intel HEX file -> image (streaming line reader) ---- */

static UpdiStatus parse_hex_file(UpdiApp* app, IntelHexImage* img) {
    File* f = storage_file_alloc(app->storage);
    UpdiStatus result = UpdiOk;
    if(!storage_file_open(f, furi_string_get_cstr(app->file_path), FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(f);
        return UpdiErrParam;
    }

    char chunk[FILE_READ_CHUNK];
    char line[HEX_LINE_MAX];
    size_t llen = 0;
    bool parse_error = false;

    size_t n;
    while((n = storage_file_read(f, chunk, sizeof(chunk))) > 0) {
        for(size_t i = 0; i < n; i++) {
            char c = chunk[i];
            if(c == '\n') {
                line[llen] = '\0';
                if(llen > 0 && intel_hex_parse_line(img, line) != IntelHexOk) {
                    parse_error = true;
                    break;
                }
                llen = 0;
            } else if(c != '\r' && llen < sizeof(line) - 1) {
                line[llen++] = c;
            }
        }
        if(parse_error) break;
    }
    if(!parse_error && llen > 0) {
        line[llen] = '\0';
        if(intel_hex_parse_line(img, line) != IntelHexOk) parse_error = true;
    }

    storage_file_close(f);
    storage_file_free(f);

    if(parse_error) result = UpdiErrParam;
    else if(!img->any) result = UpdiErrParam; /* empty image */
    return result;
}

static UpdiStatus op_flash(UpdiApp* app) {
    set_phase(app, UpdiPhaseConnecting, 0, 0);
    UpdiStatus st = do_connect(app, true);
    if(st != UpdiOk) return st;

    const UpdiDevice* dev = app->session.device;
    uint8_t* image = malloc(dev->flash_size);
    if(!image) return UpdiErrParam;
    IntelHexImage img;
    intel_hex_image_init(&img, image, dev->flash_size, 0, 0xFF);

    set_phase(app, UpdiPhaseReadingFile, 0, 0);
    st = parse_hex_file(app, &img);
    if(st != UpdiOk) {
        free(image);
        return st;
    }

    set_phase(app, UpdiPhaseErasing, 0, 0);
    st = updi_session_chip_erase(&app->session);
    if(st != UpdiOk) {
        free(image);
        return st;
    }

    set_phase(app, UpdiPhaseWriting, 0, img.max_offset);
    st = updi_session_write_flash(&app->session, image, img.max_offset, worker_progress, app);
    if(st != UpdiOk) {
        free(image);
        return st;
    }

    set_phase(app, UpdiPhaseVerifying, 0, img.max_offset);
    st = updi_session_verify_flash(
        &app->session, image, img.max_offset, worker_progress, app, &app->verify_mismatch);

    free(image);
    updi_session_disconnect(&app->session);
    return st;
}

/* ---- verify device flash against an Intel HEX (no erase, no write) ---- */

static UpdiStatus op_verify(UpdiApp* app) {
    set_phase(app, UpdiPhaseConnecting, 0, 0);
    /* Never erase during a verify: a locked device just reports UpdiErrLocked. */
    UpdiStatus st = do_connect(app, false);
    if(st != UpdiOk) return st;

    const UpdiDevice* dev = app->session.device;
    uint8_t* image = malloc(dev->flash_size);
    if(!image) return UpdiErrParam;
    IntelHexImage img;
    intel_hex_image_init(&img, image, dev->flash_size, 0, 0xFF);

    set_phase(app, UpdiPhaseReadingFile, 0, 0);
    st = parse_hex_file(app, &img);
    if(st != UpdiOk) {
        free(image);
        return st;
    }

    set_phase(app, UpdiPhaseVerifying, 0, img.max_offset);
    st = updi_session_verify_flash(
        &app->session, image, img.max_offset, worker_progress, app, &app->verify_mismatch);

    free(image);
    updi_session_disconnect(&app->session);
    return st;
}

/* ---- dump flash to Intel HEX on SD ---- */

typedef struct {
    File* file;
    bool ok;
} HexFileSink;

static bool hex_file_sink(void* ctx, const char* str, size_t len) {
    HexFileSink* s = ctx;
    if(!s->ok) return false;
    size_t written = storage_file_write(s->file, str, len);
    if(written != len) s->ok = false;
    return s->ok;
}

static UpdiStatus op_dump(UpdiApp* app) {
    set_phase(app, UpdiPhaseConnecting, 0, 0);
    UpdiStatus st = do_connect(app, false);
    if(st != UpdiOk) return st;

    const UpdiDevice* dev = app->session.device;
    uint8_t* buf = malloc(dev->flash_size);
    if(!buf) return UpdiErrParam;

    set_phase(app, UpdiPhaseReading, 0, dev->flash_size);
    st = updi_session_read_flash(&app->session, buf, dev->flash_size, worker_progress, app);
    if(st != UpdiOk) {
        free(buf);
        return st;
    }

    storage_common_mkdir(app->storage, UPDI_DUMP_DIR);
    furi_string_printf(app->file_path, "%s/%s_dump.hex", UPDI_DUMP_DIR, dev->name);

    File* f = storage_file_alloc(app->storage);
    if(!storage_file_open(
           f, furi_string_get_cstr(app->file_path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(f);
        free(buf);
        return UpdiErrParam;
    }
    HexFileSink sink = {.file = f, .ok = true};
    IntelHexStatus hs = intel_hex_write(buf, dev->flash_size, 0, 32, hex_file_sink, &sink);
    storage_file_close(f);
    storage_file_free(f);
    free(buf);

    updi_session_disconnect(&app->session);
    return (hs == IntelHexOk && sink.ok) ? UpdiOk : UpdiErrParam;
}

static UpdiStatus op_chip_erase(UpdiApp* app) {
    set_phase(app, UpdiPhaseConnecting, 0, 0);
    UpdiStatus st = do_connect(app, true);
    if(st != UpdiOk) return st;
    set_phase(app, UpdiPhaseErasing, 0, 0);
    /* If the device was locked, do_connect already chip-erased to unlock it. */
    st = updi_session_chip_erase(&app->session);
    updi_session_disconnect(&app->session);
    return st;
}

static UpdiStatus op_connect(UpdiApp* app) {
    set_phase(app, UpdiPhaseConnecting, 0, 0);
    UpdiStatus st = do_connect(app, false);
    /* Release reset so the target keeps running; device info is cached in the session. */
    if(st == UpdiOk) updi_session_disconnect(&app->session);
    return st;
}

static UpdiStatus op_read_fuses(UpdiApp* app) {
    set_phase(app, UpdiPhaseConnecting, 0, 0);
    UpdiStatus st = do_connect(app, false);
    if(st != UpdiOk) return st;
    const UpdiDevice* dev = app->session.device;
    app->fuses_count = dev->fuses_size < sizeof(app->fuses) ? dev->fuses_size : sizeof(app->fuses);
    st = updi_session_read_fuses(&app->session, app->fuses, app->fuses_count);
    updi_session_disconnect(&app->session);
    return st;
}

/* ---- thread entry ---- */

static int32_t updi_worker_thread(void* context) {
    UpdiApp* app = context;
    UpdiStatus st = UpdiErrParam;
    switch(app->op) {
    case UpdiOpConnect: st = op_connect(app); break;
    case UpdiOpChipErase: st = op_chip_erase(app); break;
    case UpdiOpFlash: st = op_flash(app); break;
    case UpdiOpVerify: st = op_verify(app); break;
    case UpdiOpDump: st = op_dump(app); break;
    case UpdiOpReadFuses: st = op_read_fuses(app); break;
    default: break;
    }

    furi_mutex_acquire(app->lock, FuriWaitForever);
    app->result = st;
    app->phase = UpdiPhaseDone;
    furi_mutex_release(app->lock);

    view_dispatcher_send_custom_event(app->view_dispatcher, UpdiCustomEventWorkerDone);
    return 0;
}

void updi_worker_start(UpdiApp* app, UpdiOp op) {
    app->op = op;
    app->result = UpdiOk;
    app->prog_done = 0;
    app->prog_total = 0;
    app->phase = UpdiPhaseConnecting;
    app->worker = furi_thread_alloc_ex("UpdiWorker", WORKER_STACK, updi_worker_thread, app);
    furi_thread_start(app->worker);
}

void updi_worker_join(UpdiApp* app) {
    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}
