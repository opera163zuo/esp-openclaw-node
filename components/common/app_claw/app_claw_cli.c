/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_cli.h"
#include "app_claw.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "esp_idf_version.h"

#include "claw_cap.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "app_claw_cli";
static const size_t CAP_OUTPUT_BUF_SIZE = 1024;

static ssize_t app_claw_cli_read_blocking(int fd, void *buffer, size_t size)
{
    for (;;) {
        ssize_t ret = read(fd, buffer, size);
        if (ret >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return ret;
        }
        vTaskDelay(1);
    }
}

static int cmd_cap_list(int argc, char **argv)
{
    claw_cap_list_t list;
    size_t i;

    (void)argc;
    (void)argv;

    list = claw_cap_list();
    if (list.count == 0) {
        printf("No capabilities registered\n");
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        const claw_cap_descriptor_t *item = &list.items[i];

        printf("%s [%s] %s\n",
               item->name,
               item->family ? item->family : "cap",
               item->description ? item->description : "");
    }

    return 0;
}

static int cmd_cap_call(int argc, char **argv)
{
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
    };

    if (argc < 3) {
        printf("Usage: cap_call <name> <json>\n");
        return 1;
    }

    {
        cJSON *json = cJSON_Parse(argv[2]);

        if (!json) {
            printf("invalid json\n");
            return 1;
        }
        cJSON_Delete(json);
    }

    output = calloc(1, CAP_OUTPUT_BUF_SIZE);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(argv[1], argv[2], &ctx, output, CAP_OUTPUT_BUF_SIZE);
    if (err == ESP_OK) {
        printf("%s\n", output);
    } else {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    }

    free(output);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_cap_groups(int argc, char **argv)
{
    claw_cap_group_list_t list;
    size_t i;

    (void)argc;
    (void)argv;

    list = claw_cap_list_groups();
    if (list.count == 0) {
        printf("No cap groups loaded\n");
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        const claw_cap_group_info_t *item = &list.items[i];

        printf("%s state=%s descriptors=%u plugin=%s version=%s\n",
               item->group_id ? item->group_id : "(null)",
               claw_cap_state_to_string(item->state),
               (unsigned)item->descriptor_count,
               item->plugin_name ? item->plugin_name : "-",
               item->version ? item->version : "-");
    }

    return 0;
}

static int cmd_cap_enable(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: cap_enable <group_id>\n");
        return 1;
    }

    err = claw_cap_enable_group(argv[1]);
    if (err != ESP_OK) {
        printf("cap_enable failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("enabled %s\n", argv[1]);
    return 0;
}

static int cmd_cap_disable(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: cap_disable <group_id>\n");
        return 1;
    }

    err = claw_cap_disable_group(argv[1]);
    if (err != ESP_OK) {
        printf("cap_disable failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("disabled %s\n", argv[1]);
    return 0;
}

static int cmd_cap(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cap <list|call|groups|enable|disable> ...\n");
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        return cmd_cap_list(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "call") == 0) {
        return cmd_cap_call(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "groups") == 0) {
        return cmd_cap_groups(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "enable") == 0) {
        return cmd_cap_enable(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "disable") == 0) {
        return cmd_cap_disable(argc - 1, &argv[1]);
    }

    printf("Unknown cap subcommand: %s\n", argv[1]);
    printf("Usage: cap <list|call|groups|enable|disable> ...\n");
    return 1;
}

esp_err_t app_claw_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "Starting console REPL");

    repl_config.prompt = "app> ";
    repl_config.task_stack_size = 10240;
    repl_config.max_cmdline_length = 512;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    linenoiseSetReadFunction(app_claw_cli_read_blocking);

    esp_console_register_help_command();

    {
        esp_console_cmd_t cap_cmd = {
            .command = "cap",
            .help = "Inspect or call local capabilities: cap <list|call|groups|enable|disable> ...",
            .func = cmd_cap,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cap_cmd));
    }

    printf("Type 'help' or 'cap list'\n");
    return esp_console_start_repl(repl);
}
