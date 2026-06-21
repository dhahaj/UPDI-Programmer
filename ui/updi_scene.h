/**
 * @file updi_scene.h
 * @brief Scene enum + handler table for the SceneManager (standard fbt scene pattern).
 *
 * The enum ids are UpdiScene<Id> and the handlers are updi_scene_<name>_on_*; both are
 * generated from updi_scene_config.h.
 */
#pragma once

#include <gui/scene_manager.h>

typedef enum {
#define ADD_SCENE(prefix, name, id) UpdiScene##id,
#include "updi_scene_config.h"
#undef ADD_SCENE
    UpdiSceneNum,
} UpdiScene;

extern const SceneManagerHandlers updi_scene_handlers;

#define ADD_SCENE(prefix, name, id)                                              \
    void updi_scene_##name##_on_enter(void* context);                            \
    bool updi_scene_##name##_on_event(void* context, SceneManagerEvent event);   \
    void updi_scene_##name##_on_exit(void* context);
#include "updi_scene_config.h"
#undef ADD_SCENE
