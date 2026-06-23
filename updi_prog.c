/**
 * @file updi_prog.c
 * @brief UPDI Programmer FAP entry point: allocates the GUI stack (ViewDispatcher +
 *        SceneManager + view modules) and runs the event loop.
 *
 * Architecture: the portable UPDI protocol lives under updi/ and talks to hardware only
 * through transport/transport_furi.c. The UI (ui/) drives a background worker thread for
 * the long operations. See README.md for wiring and the manual hardware test checklist.
 */
#include "ui/updi_app.h"
#include "ui/updi_scene.h"

static bool updi_custom_event_callback(void* context, uint32_t event) {
    UpdiApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool updi_back_event_callback(void* context) {
    UpdiApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static UpdiApp* updi_app_alloc(void) {
    UpdiApp* app = malloc(sizeof(UpdiApp));
    memset(app, 0, sizeof(UpdiApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&updi_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, updi_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, updi_back_event_callback);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, UpdiViewSubmenu, submenu_get_view(app->submenu));
    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, UpdiViewWidget, widget_get_view(app->widget));
    app->popup = popup_alloc();
    view_dispatcher_add_view(app->view_dispatcher, UpdiViewPopup, popup_get_view(app->popup));
    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        UpdiViewVarItemList,
        variable_item_list_get_view(app->var_item_list));

    app->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    app->file_path = furi_string_alloc();
    app->text = furi_string_alloc();
    app->baud = 115200;

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    return app;
}

static void updi_app_free(UpdiApp* app) {
    updi_worker_join(app); /* must finish before tearing down shared state */
    if(app->transport) transport_furi_free(app->transport);

    view_dispatcher_remove_view(app->view_dispatcher, UpdiViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, UpdiViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, UpdiViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, UpdiViewVarItemList);

    submenu_free(app->submenu);
    widget_free(app->widget);
    popup_free(app->popup);
    variable_item_list_free(app->var_item_list);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_mutex_free(app->lock);
    furi_string_free(app->file_path);
    furi_string_free(app->text);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t updi_prog_app(void* p) {
    UNUSED(p);
    UpdiApp* app = updi_app_alloc();
    scene_manager_next_scene(app->scene_manager, UpdiSceneStart);
    view_dispatcher_run(app->view_dispatcher);
    updi_app_free(app);
    return 0;
}
