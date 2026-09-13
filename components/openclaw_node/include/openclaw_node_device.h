/*
 * Board-neutral Native Node device information commands.
 * Board-specific display/audio/GPIO handlers remain outside this module.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENCLAW_NODE_DEVICE_COMMAND_INFO "device.info"
#define OPENCLAW_NODE_DEVICE_COMMAND_STATUS "device.status"
#define OPENCLAW_NODE_DEVICE_COMMAND_NETWORK "device.network"
#define OPENCLAW_NODE_DEVICE_COMMAND_BACKLIGHT "device.backlight"
#define OPENCLAW_NODE_DEVICE_COMMAND_RESTART "device.restart"
#define OPENCLAW_NODE_DEVICE_COMMAND_BUTTON_STATUS "device.button.status"
#define OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_VOLUME "audio.volume"
#define OPENCLAW_NODE_DEVICE_COMMAND_AUDIO_TONE "audio.tone"
#define OPENCLAW_NODE_DEVICE_COMMAND_SCREEN_CLEAR "device.screen.clear"
#define OPENCLAW_NODE_COMMAND_FILES_READ "node.files.read"
#define OPENCLAW_NODE_COMMAND_FILES_WRITE "node.files.write"
#define OPENCLAW_NODE_COMMAND_FILES_DELETE "node.files.delete"
#define OPENCLAW_NODE_COMMAND_FILES_COPY "node.files.copy"
#define OPENCLAW_NODE_COMMAND_FILES_MOVE "node.files.move"
#define OPENCLAW_NODE_COMMAND_FILES_LIST "node.files.list"
#define OPENCLAW_NODE_COMMAND_LUA_RUN "node.lua.run"
#define OPENCLAW_NODE_COMMAND_LUA_RUN_ASYNC "node.lua.run_async"
#define OPENCLAW_NODE_COMMAND_LUA_JOBS "node.lua.jobs"
#define OPENCLAW_NODE_COMMAND_LUA_JOB "node.lua.job"
#define OPENCLAW_NODE_COMMAND_LUA_STOP "node.lua.stop"
#define OPENCLAW_NODE_COMMAND_LUA_STOP_ALL "node.lua.stop_all"

/** Return the static command names supplied by this module. */
const char *const *openclaw_node_device_commands(size_t *count);

/** Handle device.info and device.status. */
esp_err_t openclaw_node_device_command(const char *command,
                                       const char *params_json,
                                       char *result_json,
                                       size_t result_size,
                                       void *user_ctx);

#ifdef __cplusplus
}
#endif
