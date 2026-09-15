/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "app_claw.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Resolved storage paths threaded through the capability framework
 *
 *         Built inside app_claw from the logical homes registered in claw_paths
 *         (see claw_paths.h); not part of the public app_claw API. Each field is
 *         an absolute path derived from a home plus a fixed subdirectory.
 */
typedef struct {
    char fatfs_base_path[APP_CLAW_PATH_LEN];  /**< Writable data root */
    char lua_root_dir[APP_CLAW_PATH_LEN];     /**< User Lua scripts root */
} app_claw_storage_paths_t;

#ifdef __cplusplus
}
#endif
