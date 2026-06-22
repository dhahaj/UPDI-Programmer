/**
 * @file updi_app.h
 * @brief Flipper app state and shared declarations for the UPDI Programmer.
 */
#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/popup.h>
#include <gui/modules/variable_item_list.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#include "../updi/updi_session.h"
#include "../transport/transport_furi.h"
#include "../hex/intel_hex.h"

#define UPDI_TAG "UpdiProg"
#define UPDI_DUMP_DIR EXT_PATH("updi")

/* View ids registered with the ViewDispatcher. */
typedef enum {
    UpdiViewSubmenu,
    UpdiViewWidget,
    UpdiViewPopup,
    UpdiViewVarItemList,
} UpdiViewId;

/* Long-running operations executed on the worker thread. */
typedef enum {
    UpdiOpNone,
    UpdiOpConnect,
    UpdiOpChipErase,
    UpdiOpFlash,
    UpdiOpVerify,
    UpdiOpDump,
    UpdiOpReadFuses,
} UpdiOp;

/* Custom ViewDispatcher events. Submenu item ids reuse the low range. */
typedef enum {
    UpdiCustomEventTick = 1000,
    UpdiCustomEventWorkerDone,
} UpdiCustomEvent;

/* Phase shown on the progress screen. */
typedef enum {
    UpdiPhaseConnecting,
    UpdiPhaseReadingFile,
    UpdiPhaseErasing,
    UpdiPhaseWriting,
    UpdiPhaseVerifying,
    UpdiPhaseReading,
    UpdiPhaseDone,
} UpdiPhase;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Storage* storage;
    DialogsApp* dialogs;
    NotificationApp* notifications;

    Submenu* submenu;
    Widget* widget;
    Popup* popup;
    VariableItemList* var_item_list;

    /* UPDI stack (transport acquired lazily on first connect) */
    TransportFuri* transport;
    UpdiSession session;
    bool have_device; /* a device was successfully identified at least once */

    /* settings */
    uint32_t baud;

    /* worker */
    FuriThread* worker;
    UpdiOp op;
    FuriMutex* lock; /* guards the progress fields below */
    UpdiPhase phase;
    size_t prog_done;
    size_t prog_total;
    UpdiStatus result; /* status of the last finished op */
    UpdiVerifyMismatch verify_mismatch; /* first differing byte when result == UpdiErrVerify */
    FuriTimer* timer;

    /* shared text */
    FuriString* file_path; /* selected HEX (flash) or written dump path */
    FuriString* text; /* scratch for device info / result / about screens */

    /* fuse readout (filled by UpdiOpReadFuses) */
    uint8_t fuses[16];
    size_t fuses_count;

    /* persistent buffer for popup text — popup_set_text() stores the pointer, not a copy */
    char popup_text[48];
} UpdiApp;

/* Worker entry (ui/updi_worker.c). Starts the FuriThread for app->op. */
void updi_worker_start(UpdiApp* app, UpdiOp op);
void updi_worker_join(UpdiApp* app);

/* Human-readable status string for messages. */
const char* updi_status_str(UpdiStatus s);
