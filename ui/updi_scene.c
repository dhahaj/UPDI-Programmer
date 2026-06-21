/**
 * @file updi_scene.c
 * @brief Builds the SceneManager handler tables from the scene config list.
 */
#include "updi_scene.h"

void (*const updi_scene_on_enter_handlers[])(void*) = {
#define ADD_SCENE(prefix, name, id) updi_scene_##name##_on_enter,
#include "updi_scene_config.h"
#undef ADD_SCENE
};

bool (*const updi_scene_on_event_handlers[])(void*, SceneManagerEvent) = {
#define ADD_SCENE(prefix, name, id) updi_scene_##name##_on_event,
#include "updi_scene_config.h"
#undef ADD_SCENE
};

void (*const updi_scene_on_exit_handlers[])(void*) = {
#define ADD_SCENE(prefix, name, id) updi_scene_##name##_on_exit,
#include "updi_scene_config.h"
#undef ADD_SCENE
};

const SceneManagerHandlers updi_scene_handlers = {
    .on_enter_handlers = updi_scene_on_enter_handlers,
    .on_event_handlers = updi_scene_on_event_handlers,
    .on_exit_handlers = updi_scene_on_exit_handlers,
    .scene_num = UpdiSceneNum,
};
