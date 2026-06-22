/**
 * @file updi_scenes.c
 * @brief All SceneManager scenes for the UPDI Programmer (start menu, device info,
 *        progress, result, fuses, settings, about).
 */
#include "updi_app.h"
#include "updi_scene.h"

#include <gui/elements.h>

/* ============================ Start (main menu) ============================ */

typedef enum {
    StartItemConnect,
    StartItemFlash,
    StartItemVerify,
    StartItemDump,
    StartItemErase,
    StartItemFuses,
    StartItemSettings,
    StartItemAbout,
} StartItem;

static void start_submenu_cb(void* context, uint32_t index) {
    UpdiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static bool pick_hex_file(UpdiApp* app) {
    DialogsFileBrowserOptions opts;
    dialog_file_browser_set_basic_options(&opts, ".hex", NULL);
    opts.base_path = STORAGE_EXT_PATH_PREFIX;
    furi_string_set(app->file_path, STORAGE_EXT_PATH_PREFIX);
    return dialog_file_browser_show(app->dialogs, app->file_path, app->file_path, &opts);
}

void updi_scene_start_on_enter(void* context) {
    UpdiApp* app = context;
    Submenu* m = app->submenu;
    submenu_reset(m);
    submenu_set_header(m, "UPDI Programmer");
    submenu_add_item(m, "Connect / Read Device", StartItemConnect, start_submenu_cb, app);
    submenu_add_item(m, "Flash from HEX", StartItemFlash, start_submenu_cb, app);
    submenu_add_item(m, "Verify from HEX", StartItemVerify, start_submenu_cb, app);
    submenu_add_item(m, "Dump Flash", StartItemDump, start_submenu_cb, app);
    submenu_add_item(m, "Chip Erase", StartItemErase, start_submenu_cb, app);
    submenu_add_item(m, "Read Fuses", StartItemFuses, start_submenu_cb, app);
    submenu_add_item(m, "Settings", StartItemSettings, start_submenu_cb, app);
    submenu_add_item(m, "About / Wiring", StartItemAbout, start_submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewSubmenu);
}

bool updi_scene_start_on_event(void* context, SceneManagerEvent event) {
    UpdiApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    switch(event.event) {
    case StartItemConnect:
        app->op = UpdiOpConnect;
        scene_manager_next_scene(app->scene_manager, UpdiSceneProgress);
        return true;
    case StartItemFlash:
        if(pick_hex_file(app)) {
            app->op = UpdiOpFlash;
            scene_manager_next_scene(app->scene_manager, UpdiSceneProgress);
        }
        return true;
    case StartItemVerify:
        if(pick_hex_file(app)) {
            app->op = UpdiOpVerify;
            scene_manager_next_scene(app->scene_manager, UpdiSceneProgress);
        }
        return true;
    case StartItemDump:
        app->op = UpdiOpDump;
        scene_manager_next_scene(app->scene_manager, UpdiSceneProgress);
        return true;
    case StartItemErase:
        app->op = UpdiOpChipErase;
        scene_manager_next_scene(app->scene_manager, UpdiSceneProgress);
        return true;
    case StartItemFuses:
        app->op = UpdiOpReadFuses;
        scene_manager_next_scene(app->scene_manager, UpdiSceneProgress);
        return true;
    case StartItemSettings:
        scene_manager_next_scene(app->scene_manager, UpdiSceneSettings);
        return true;
    case StartItemAbout:
        scene_manager_next_scene(app->scene_manager, UpdiSceneAbout);
        return true;
    default:
        return false;
    }
}

void updi_scene_start_on_exit(void* context) {
    UpdiApp* app = context;
    submenu_reset(app->submenu);
}

/* ============================ Device info ============================ */

void updi_scene_device_info_on_enter(void* context) {
    UpdiApp* app = context;
    const UpdiDevice* d = app->session.device;
    const UpdiSibInfo* sib = &app->session.sib;
    const uint8_t* sig = app->session.sig;

    furi_string_reset(app->text);
    if(d) {
        furi_string_printf(
            app->text,
            "Device: %s\n"
            "Signature: %02X %02X %02X\n"
            "Family: %s  (NVM P:%u)\n"
            "Flash: %lu B, page %u\n"
            "EEPROM: %u B\n"
            "Fuses: %u\n"
            "Addressing: %u-bit",
            d->name,
            sig[0],
            sig[1],
            sig[2],
            sib->family,
            (unsigned)sib->nvm_version,
            (unsigned long)d->flash_size,
            (unsigned)d->flash_page_size,
            (unsigned)d->eeprom_size,
            (unsigned)d->fuses_size,
            (unsigned)(d->address_bytes * 8));
    } else {
        furi_string_set(app->text, "No device identified.");
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Device Info");
    widget_add_text_scroll_element(
        app->widget, 0, 13, 128, 51, furi_string_get_cstr(app->text));
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewWidget);
}

bool updi_scene_device_info_on_event(void* context, SceneManagerEvent event) {
    UpdiApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, UpdiSceneStart);
        return true;
    }
    return false;
}

void updi_scene_device_info_on_exit(void* context) {
    UpdiApp* app = context;
    widget_reset(app->widget);
}

/* ============================ Progress ============================ */

static const char* phase_name(UpdiPhase p) {
    switch(p) {
    case UpdiPhaseConnecting: return "Connecting";
    case UpdiPhaseReadingFile: return "Reading file";
    case UpdiPhaseErasing: return "Erasing";
    case UpdiPhaseWriting: return "Writing";
    case UpdiPhaseVerifying: return "Verifying";
    case UpdiPhaseReading: return "Reading";
    default: return "Done";
    }
}

static void progress_timer_cb(void* context) {
    UpdiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, UpdiCustomEventTick);
}

void updi_scene_progress_on_enter(void* context) {
    UpdiApp* app = context;
    popup_reset(app->popup);
    popup_set_header(app->popup, "Working", 64, 6, AlignCenter, AlignTop);
    popup_set_text(app->popup, "Starting...", 64, 34, AlignCenter, AlignCenter);
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewPopup);

    updi_worker_start(app, app->op);
    app->timer = furi_timer_alloc(progress_timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(120));
}

bool updi_scene_progress_on_event(void* context, SceneManagerEvent event) {
    UpdiApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        return true; /* block back while the worker runs */
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == UpdiCustomEventTick) {
        furi_mutex_acquire(app->lock, FuriWaitForever);
        UpdiPhase ph = app->phase;
        size_t done = app->prog_done, total = app->prog_total;
        furi_mutex_release(app->lock);
        /* popup_set_text() keeps the pointer, so write into the app-owned buffer. */
        if(total > 0) {
            snprintf(
                app->popup_text,
                sizeof(app->popup_text),
                "%s\n%u%%",
                phase_name(ph),
                (unsigned)(done * 100 / total));
        } else {
            snprintf(app->popup_text, sizeof(app->popup_text), "%s...", phase_name(ph));
        }
        popup_set_text(app->popup, app->popup_text, 64, 34, AlignCenter, AlignCenter);
        return true;
    }

    if(event.event == UpdiCustomEventWorkerDone) {
        if(app->timer) {
            furi_timer_stop(app->timer);
            furi_timer_free(app->timer);
            app->timer = NULL;
        }
        updi_worker_join(app);
        notification_message(
            app->notifications, app->result == UpdiOk ? &sequence_success : &sequence_error);

        if(app->result == UpdiOk && app->op == UpdiOpConnect) {
            scene_manager_next_scene(app->scene_manager, UpdiSceneDeviceInfo);
        } else if(app->result == UpdiOk && app->op == UpdiOpReadFuses) {
            scene_manager_next_scene(app->scene_manager, UpdiSceneFuses);
        } else {
            scene_manager_next_scene(app->scene_manager, UpdiSceneResult);
        }
        return true;
    }
    return false;
}

void updi_scene_progress_on_exit(void* context) {
    UpdiApp* app = context;
    if(app->timer) {
        furi_timer_stop(app->timer);
        furi_timer_free(app->timer);
        app->timer = NULL;
    }
    updi_worker_join(app); /* safety: ensure the thread is gone */
    popup_reset(app->popup);
}

/* ============================ Result ============================ */

static const char* op_name(UpdiOp op) {
    switch(op) {
    case UpdiOpConnect: return "Connect";
    case UpdiOpChipErase: return "Chip erase";
    case UpdiOpFlash: return "Flash";
    case UpdiOpVerify: return "Verify";
    case UpdiOpDump: return "Dump";
    case UpdiOpReadFuses: return "Read fuses";
    default: return "Operation";
    }
}

void updi_scene_result_on_enter(void* context) {
    UpdiApp* app = context;
    furi_string_reset(app->text);

    if(app->result == UpdiOk) {
        switch(app->op) {
        case UpdiOpFlash:
            furi_string_set(app->text, "Write + read-back verify passed.");
            break;
        case UpdiOpVerify:
            furi_string_set(app->text, "Verify passed.\nDevice matches HEX.");
            break;
        case UpdiOpChipErase:
            furi_string_set(app->text, "Chip erased.");
            break;
        case UpdiOpDump:
            furi_string_printf(
                app->text, "Saved:\n%s", furi_string_get_cstr(app->file_path));
            break;
        default:
            furi_string_printf(app->text, "%s OK", op_name(app->op));
            break;
        }
    } else {
        furi_string_printf(
            app->text, "%s failed:\n%s", op_name(app->op), updi_status_str(app->result));
        if(app->result == UpdiErrVerify)
            furi_string_cat_printf(
                app->text,
                "\nFirst diff at 0x%04X:\nHEX 0x%02X vs chip 0x%02X",
                (unsigned)app->verify_mismatch.offset,
                app->verify_mismatch.expected,
                app->verify_mismatch.actual);
        else if(app->result == UpdiErrLocked)
            furi_string_cat_str(app->text, "\nUse Chip Erase to unlock (erases all).");
        else if(app->result == UpdiErrTimeout)
            furi_string_cat_str(app->text, "\nCheck wiring and 3.3 V power.");
        else if(app->result == UpdiErrUnsupported)
            furi_string_cat_str(app->text, "\nSignature not in device table.");
        else if(app->result == UpdiErrTransport)
            furi_string_cat_str(app->text, "\nUSART busy? Close other GPIO apps.");
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget,
        64,
        0,
        AlignCenter,
        AlignTop,
        FontPrimary,
        app->result == UpdiOk ? "Success" : "Error");
    widget_add_text_scroll_element(
        app->widget, 0, 14, 128, 50, furi_string_get_cstr(app->text));
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewWidget);
}

bool updi_scene_result_on_event(void* context, SceneManagerEvent event) {
    UpdiApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, UpdiSceneStart);
        return true;
    }
    return false;
}

void updi_scene_result_on_exit(void* context) {
    UpdiApp* app = context;
    widget_reset(app->widget);
}

/* ============================ Fuses ============================ */

void updi_scene_fuses_on_enter(void* context) {
    UpdiApp* app = context;
    furi_string_reset(app->text);
    const UpdiDevice* d = app->session.device;
    furi_string_cat_printf(app->text, "%s fuses:\n", d ? d->name : "");
    for(size_t i = 0; i < app->fuses_count; i++) {
        furi_string_cat_printf(app->text, "F%u: 0x%02X\n", (unsigned)i, app->fuses[i]);
    }
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "Fuses");
    widget_add_text_scroll_element(
        app->widget, 0, 13, 128, 51, furi_string_get_cstr(app->text));
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewWidget);
}

bool updi_scene_fuses_on_event(void* context, SceneManagerEvent event) {
    UpdiApp* app = context;
    if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, UpdiSceneStart);
        return true;
    }
    return false;
}

void updi_scene_fuses_on_exit(void* context) {
    UpdiApp* app = context;
    widget_reset(app->widget);
}

/* ============================ Settings ============================ */

static const uint32_t baud_values[] = {115200, 230400, 57600, 38400, 19200};
static const char* const baud_names[] = {"115200", "230400", "57600", "38400", "19200"};
#define BAUD_COUNT (sizeof(baud_values) / sizeof(baud_values[0]))

static void settings_baud_changed(VariableItem* item) {
    UpdiApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, baud_names[idx]);
    app->baud = baud_values[idx];
    if(app->transport) transport_furi_set_baud(app->transport, app->baud);
}

void updi_scene_settings_on_enter(void* context) {
    UpdiApp* app = context;
    VariableItemList* list = app->var_item_list;
    variable_item_list_reset(list);
    variable_item_list_set_header(list, "Settings");

    VariableItem* item =
        variable_item_list_add(list, "Baud", BAUD_COUNT, settings_baud_changed, app);
    uint8_t idx = 0;
    for(uint8_t i = 0; i < BAUD_COUNT; i++) {
        if(baud_values[i] == app->baud) {
            idx = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, idx);
    variable_item_set_current_value_text(item, baud_names[idx]);

    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewVarItemList);
}

bool updi_scene_settings_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false; /* back: default -> previous (start) */
}

void updi_scene_settings_on_exit(void* context) {
    UpdiApp* app = context;
    variable_item_list_reset(app->var_item_list);
}

/* ============================ About / Wiring ============================ */

void updi_scene_about_on_enter(void* context) {
    UpdiApp* app = context;
    furi_string_set(
        app->text,
        "UPDI Programmer\n"
        "tinyAVR 0/1/2, megaAVR 0,\nAVR EA. 3.3 V UPDI only.\n"
        "\n"
        "Wiring (resistor bridge):\n"
        "Pin 13 (TX) -1k- UPDI node\n"
        "Pin 14 (RX) --- UPDI node\n"
        "UPDI node ---- target UPDI\n"
        "Pin 9 (3V3) -- target VCC\n"
        "Pin 11/18 GND - target GND\n"
        "(opt) 4.7-10k pull-up\n"
        "UPDI node -> 3V3\n"
        "\n"
        "WARNINGS:\n"
        "- Power target at 3.3 V only.\n"
        "  5 V needs a level shifter.\n"
        "- No High-Voltage UPDI: a\n"
        "  UPDI pin fused to RESET/GPIO\n"
        "  needs a 12 V programmer.\n"
        "\n"
        "Firmware: Momentum mntm-012\n"
        "https://github.com/dhahaj/UPDI-Programmer\n");
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 0, AlignCenter, AlignTop, FontPrimary, "About");
    widget_add_text_scroll_element(
        app->widget, 0, 13, 128, 51, furi_string_get_cstr(app->text));
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewWidget);
}

bool updi_scene_about_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void updi_scene_about_on_exit(void* context) {
    UpdiApp* app = context;
    widget_reset(app->widget);
}
