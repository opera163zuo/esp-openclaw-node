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
