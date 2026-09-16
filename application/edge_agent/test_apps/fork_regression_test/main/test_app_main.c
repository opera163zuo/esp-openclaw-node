/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-target regression tests for the fork's changes.
 *
 * Two groups:
 *
 *   [app_config]     the in-memory config cache must never hand back a stale
 *                    value, and a loaded copy must not be able to mutate the
 *                    store behind the next caller's back.
 *
 *   [openclaw_node]  the transport must survive repeated start/stop without
 *                    leaking, and must keep servicing the socket while a slow
 *                    command runs. That second property is the point of running
 *                    commands on a worker: the WebSocket task owns the receive
 *                    loop, so blocking it inside a command stalls the
 *                    connection until the Gateway gives up on the node.
 *
 * The transport tests need no Gateway. They run a WebSocket server on the device
 * itself and point the node at it over loopback, so they need neither Wi-Fi nor
 * a board. The evidence that the socket stays live is the PING/PONG traffic:
 * esp_transport_ws answers a PING from inside the client's receive loop, so a
 * WebSocket task that is stuck in a command answers none of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http_server.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "openclaw_node.h"
#include "time_sync.h"
#include "unity.h"
#include "unity_test_runner.h"

static const char *TAG = "fork_regression";

/* ── app_config: the in-memory cache ───────────────────────────────────── */

/* Both structs are ~4 KB, so they are static rather than stacked. */
static app_config_t s_config_a;
static app_config_t s_config_b;

static void config_group_setup(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(app_config_init());
}

TEST_CASE("app_config: load returns what save stored", "[app_config]")
{
    memset(&s_config_a, 0, sizeof(s_config_a));
    app_config_load_defaults(&s_config_a);
    strlcpy(s_config_a.wifi_ssid, "cache-ssid", sizeof(s_config_a.wifi_ssid));
    strlcpy(s_config_a.portal_password, "cache-portal", sizeof(s_config_a.portal_password));
    strlcpy(s_config_a.openclaw_gateway_url, "ws://cache.invalid/ws",
            sizeof(s_config_a.openclaw_gateway_url));

    TEST_ASSERT_EQUAL(ESP_OK, app_config_save(&s_config_a));

    memset(&s_config_b, 0, sizeof(s_config_b));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_b));
    TEST_ASSERT_EQUAL_STRING("cache-ssid", s_config_b.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("cache-portal", s_config_b.portal_password);
    TEST_ASSERT_EQUAL_STRING("ws://cache.invalid/ws", s_config_b.openclaw_gateway_url);
}

/* The cache introduces exactly one new failure mode: a write stops being
 * observed. Before it existed this held trivially, because every load walked
 * NVS. The two writers below stand in for the portal and the CLI's `wifi set`,
 * which is the pair that would silently revert each other's changes. */
TEST_CASE("app_config: a save is visible to the very next load", "[app_config]")
{
    memset(&s_config_a, 0, sizeof(s_config_a));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_a));
    strlcpy(s_config_a.wifi_ssid, "written-by-cli", sizeof(s_config_a.wifi_ssid));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_save(&s_config_a));

    memset(&s_config_b, 0, sizeof(s_config_b));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_b));
    TEST_ASSERT_EQUAL_STRING("written-by-cli", s_config_b.wifi_ssid);

    /* And back the other way, as the portal would do it. */
    strlcpy(s_config_b.wifi_ssid, "written-by-portal", sizeof(s_config_b.wifi_ssid));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_save(&s_config_b));

    memset(&s_config_a, 0, sizeof(s_config_a));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_a));
    TEST_ASSERT_EQUAL_STRING("written-by-portal", s_config_a.wifi_ssid);
}

/* load() must hand back a copy. If it returned the cached struct itself, a
 * caller editing the result in place would silently rewrite the stored config
 * for everyone else. */
TEST_CASE("app_config: a loaded copy cannot mutate the store", "[app_config]")
{
    memset(&s_config_a, 0, sizeof(s_config_a));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_a));
    strlcpy(s_config_a.wifi_ssid, "pinned-value", sizeof(s_config_a.wifi_ssid));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_save(&s_config_a));

    memset(&s_config_b, 0, sizeof(s_config_b));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_b));
    strlcpy(s_config_b.wifi_ssid, "local-edit-never-saved", sizeof(s_config_b.wifi_ssid));

    memset(&s_config_a, 0, sizeof(s_config_a));
    TEST_ASSERT_EQUAL(ESP_OK, app_config_load(&s_config_a));
    TEST_ASSERT_EQUAL_STRING("pinned-value", s_config_a.wifi_ssid);
}

/* ── openclaw_node: the command callback used by the tests ─────────────── */

#define TEST_COMMAND_ECHO  "test.echo"
#define TEST_COMMAND_SLEEP "test.sleep"
/* Used by the drain-window test. It keeps its own counter so a worker left
 * running past stop() cannot disturb the other cases. */
#define TEST_COMMAND_LONG  "test.long"

static const char *s_test_commands[] = {
    TEST_COMMAND_ECHO, TEST_COMMAND_SLEEP, TEST_COMMAND_LONG,
};

static volatile int32_t s_command_sleep_ms;
static volatile int     s_command_calls;
static volatile int     s_long_command_started;
static volatile int     s_long_command_finished;

static esp_err_t test_sign_cb(const char *payload, char *signature_out,
                              size_t signature_size, void *user_ctx)
{
    (void)payload;
    (void)user_ctx;
    if (!signature_out || signature_size < 16) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(signature_out, "test-signature-value", signature_size);
    return ESP_OK;
}

static esp_err_t test_command_cb(const char *command, const char *params_json,
                                 char *result_json, size_t result_size, void *user_ctx)
{
    (void)params_json;
    (void)user_ctx;

    if (strcmp(command, TEST_COMMAND_LONG) == 0) {
        s_long_command_started = 1;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)s_command_sleep_ms));
        s_long_command_finished = 1;
        snprintf(result_json, result_size, "{\"ran\":\"%s\"}", command);
        return ESP_OK;
    }

    s_command_calls++;
    if (strcmp(command, TEST_COMMAND_SLEEP) == 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)s_command_sleep_ms));
    }
    snprintf(result_json, result_size, "{\"ran\":\"%s\"}", command);
    return ESP_OK;
}

static openclaw_node_config_t make_node_config(const char *gateway_url)
{
    const openclaw_node_config_t config = {
        .gateway_url = gateway_url,
        .gateway_token = NULL,
        .device_id = "test-device-id",
        .public_key_b64url = "test-public-key",
        .client_id = "node-host",
        .client_version = "0.1.0",
        .platform = "esp32",
        .device_family = "test",
        .commands = s_test_commands,
        .command_count = sizeof(s_test_commands) / sizeof(s_test_commands[0]),
        .sign_cb = test_sign_cb,
        .command_cb = test_command_cb,
        .user_ctx = NULL,
    };
    return config;
}

/* ── openclaw_node: lifecycle ──────────────────────────────────────────── */

TEST_CASE("openclaw_node: start rejects a null config", "[openclaw_node]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, openclaw_node_start(NULL));
}

TEST_CASE("openclaw_node: stop before start is a no-op", "[openclaw_node]")
{
    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());
    TEST_ASSERT_FALSE(openclaw_node_is_connected());
}

TEST_CASE("openclaw_node: start twice is rejected", "[openclaw_node]")
{
    openclaw_node_config_t config = make_node_config("ws://127.0.0.1:1/ws");

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());
}

/* The command worker, its queue, its semaphores and the reply lock are created
 * per start() and released by stop(). A leak there would only surface after
 * many Gateway reconnects, so cycle and watch the heap. Each WebSocket client
 * allocates an 8 KB task stack, so a missing teardown costs kilobytes per cycle
 * rather than the allocator noise allowed for below. */
TEST_CASE("openclaw_node: repeated start/stop cycles do not leak", "[openclaw_node]")
{
    enum { CYCLES = 8 };
    openclaw_node_config_t config = make_node_config("ws://127.0.0.1:1/ws");

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());
    vTaskDelay(pdMS_TO_TICKS(200));

    size_t baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    for (int i = 0; i < CYCLES; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
        TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "internal heap: baseline=%u after_%d_cycles=%u (delta=%d)",
             (unsigned)baseline, CYCLES, (unsigned)after, (int)after - (int)baseline);

    TEST_ASSERT_TRUE_MESSAGE(after + 2048 >= baseline,
                             "internal heap shrank across start/stop cycles");
    TEST_ASSERT_FALSE(openclaw_node_is_connected());
}

/* ── openclaw_node: a fake Gateway on the device itself ────────────────── */

#define TEST_GW_PORT            8090
#define TEST_GW_PATH            "/ws"
#define TEST_SLOW_ID            "inv-slow"
#define TEST_FAST_ID            "inv-fast"
#define TEST_SPLIT_ID           "inv-split"
/* Comfortably past the 125-byte threshold, so the frame uses a 16-bit length and
 * has enough body to be worth splitting. */
#define TEST_SPLIT_FRAME_LEN    900
/* Past the node's 24576-byte receive buffer, which is what makes it refuse. */
#define TEST_OVERSIZED_FRAME_LEN 30000
#define TEST_SLOW_COMMAND_MS    2500
#define TEST_PING_INTERVAL_MS   200
#define TEST_TEXT_MAX           2048
#define TEST_MAX_REPLIES        4

#define GW_UPGRADED_BIT     BIT0
#define GW_CONNECT_BIT      BIT1
#define GW_SLOW_REPLY_BIT   BIT2
#define GW_FAST_REPLY_BIT   BIT3
#define GW_SPLIT_REPLY_BIT  BIT4

/* Which script the fake gateway plays. */
typedef enum {
    GW_MODE_QUEUE_ORDER,      /* a slow invoke with a fast one queued behind it */
    GW_MODE_LONG_COMMAND,     /* one command that outlasts the drain window */
    GW_MODE_SPLIT_FRAME,      /* one invoke whose frame is written in two pieces */
    GW_MODE_OVERSIZED_FRAME,  /* a frame bigger than the node's receive buffer */
} gw_mode_t;

static gw_mode_t s_gw_mode;

typedef struct {
    int     pong_count;                 /* pongs seen since the reset */
    int     reply_count;
    char    reply_order[TEST_MAX_REPLIES][32];
    char    slow_reply[TEST_TEXT_MAX];
    char    fast_reply[TEST_TEXT_MAX];
    char    split_reply[TEST_TEXT_MAX];
} gw_stats_t;

static gw_stats_t s_gw;
static httpd_handle_t s_gw_server;
static EventGroupHandle_t s_gw_events;
static volatile int s_gw_fd = -1;
static char s_gw_rx[TEST_TEXT_MAX];

static void gw_record_reply(const char *id)
{
    if (s_gw.reply_count < TEST_MAX_REPLIES) {
        strlcpy(s_gw.reply_order[s_gw.reply_count], id,
                sizeof(s_gw.reply_order[0]));
    }
    s_gw.reply_count++;

    if (strcmp(id, TEST_SLOW_ID) == 0) {
        strlcpy(s_gw.slow_reply, s_gw_rx, sizeof(s_gw.slow_reply));
        xEventGroupSetBits(s_gw_events, GW_SLOW_REPLY_BIT);
    } else if (strcmp(id, TEST_FAST_ID) == 0) {
        strlcpy(s_gw.fast_reply, s_gw_rx, sizeof(s_gw.fast_reply));
        xEventGroupSetBits(s_gw_events, GW_FAST_REPLY_BIT);
    } else if (strcmp(id, TEST_SPLIT_ID) == 0) {
        strlcpy(s_gw.split_reply, s_gw_rx, sizeof(s_gw.split_reply));
        xEventGroupSetBits(s_gw_events, GW_SPLIT_REPLY_BIT);
    }
}

static esp_err_t gw_ws_handler(httpd_req_t *req)
{
    /* The first call on a connection is the upgrade itself. */
    if (req->method == HTTP_GET && s_gw_fd < 0) {
        s_gw_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "fake gateway: upgraded, fd=%d", s_gw_fd);
        xEventGroupSetBits(s_gw_events, GW_UPGRADED_BIT);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fake gateway: frame header read failed: %s", esp_err_to_name(err));
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_PONG) {
        s_gw.pong_count++;
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGW(TAG, "fake gateway: client closed the connection");
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0) {
        return ESP_OK;
    }

    size_t len = frame.len < sizeof(s_gw_rx) - 1 ? frame.len : sizeof(s_gw_rx) - 1;
    frame.payload = (uint8_t *)s_gw_rx;
    err = httpd_ws_recv_frame(req, &frame, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fake gateway: frame body read failed: %s", esp_err_to_name(err));
        return err;
    }
    s_gw_rx[len] = '\0';

    cJSON *root = cJSON_Parse(s_gw_rx);
    if (root) {
        const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(root, "method"));
        if (method && strcmp(method, "connect") == 0) {
            xEventGroupSetBits(s_gw_events, GW_CONNECT_BIT);
        } else if (method && strcmp(method, "node.invoke.result") == 0) {
            cJSON *params = cJSON_GetObjectItem(root, "params");
            const char *id = params ? cJSON_GetStringValue(cJSON_GetObjectItem(params, "id")) : NULL;
            if (id) {
                gw_record_reply(id);
            }
        }
        cJSON_Delete(root);
    }
    return ESP_OK;
}

static esp_err_t gw_send(httpd_ws_type_t type, const char *payload, size_t len)
{
    if (s_gw_fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_ws_frame_t frame = {
        .type = type,
        .payload = (uint8_t *)payload,
        .len = len,
        .final = true,
    };
    return httpd_ws_send_frame_async(s_gw_server, s_gw_fd, &frame);
}

static esp_err_t gw_send_invoke(const char *id, const char *command)
{
    char invoke[256];
    snprintf(invoke, sizeof(invoke),
             "{\"type\":\"event\",\"event\":\"node.invoke.request\","
             "\"payload\":{\"id\":\"%s\",\"command\":\"%s\",\"paramsJSON\":\"{}\"}}",
             id, command);
    return gw_send(HTTPD_WS_TYPE_TEXT, invoke, strlen(invoke));
}

/* Waits for the upgrade, sends the challenge, then waits for the node's connect
 * request - which it only sends once it has the challenge. */
static bool gw_complete_handshake(void)
{
    const char *challenge =
        "{\"type\":\"event\",\"event\":\"connect.challenge\","
        "\"payload\":{\"nonce\":\"test-nonce\",\"ts\":1700000000000}}";

    if ((xEventGroupWaitBits(s_gw_events, GW_UPGRADED_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(5000)) & GW_UPGRADED_BIT) == 0) {
        ESP_LOGE(TAG, "fake gateway: no client connected in time");
        return false;
    }
    if (gw_send(HTTPD_WS_TYPE_TEXT, challenge, strlen(challenge)) != ESP_OK) {
        ESP_LOGE(TAG, "fake gateway: challenge send failed");
        return false;
    }
    if ((xEventGroupWaitBits(s_gw_events, GW_CONNECT_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(5000)) & GW_CONNECT_BIT) == 0) {
        ESP_LOGE(TAG, "fake gateway: node never sent a connect request");
        return false;
    }
    ESP_LOGI(TAG, "fake gateway: node connected");
    return true;
}

/* Writes one text frame by hand so it can be split across two send() calls.
 * httpd_ws_send_frame_async always writes a whole frame, and splitting is the
 * only way to make the client hand the node more than one event for a single
 * frame. Server-to-client frames are unmasked, so the header is just the opcode
 * and the length. */
static bool gw_send_text_frame_split(const char *payload, size_t len,
                                     size_t split_at, uint32_t gap_ms)
{
    if (s_gw_fd < 0 || len > 0xFFFF) {
        return false;
    }

    uint8_t header[4];
    size_t header_len;
    header[0] = 0x81;                       /* FIN | text */
    if (len <= 125) {
        header[1] = (uint8_t)len;
        header_len = 2;
    } else {
        header[1] = 126;                    /* 16-bit length follows */
        header[2] = (uint8_t)(len >> 8);
        header[3] = (uint8_t)(len & 0xFF);
        header_len = 4;
    }

    if (send(s_gw_fd, header, header_len, 0) != (int)header_len) {
        return false;
    }
    if (split_at > len) {
        split_at = len;
    }
    if (split_at > 0 && send(s_gw_fd, payload, split_at, 0) != (int)split_at) {
        return false;
    }
    if (gap_ms) {
        vTaskDelay(pdMS_TO_TICKS(gap_ms));
    }
    const size_t rest = len - split_at;
    if (rest > 0 && send(s_gw_fd, payload + split_at, rest, 0) != (int)rest) {
        return false;
    }
    return true;
}

/* Drives one scripted session. */
static void fake_gateway_task(void *arg)
{
    (void)arg;

    if (!gw_complete_handshake()) {
        vTaskDelete(NULL);
        return;
    }

    /* Measurement starts here: count pongs while the slow command runs, and put
     * the second request behind the first so the queue has to hold it. */
    s_gw.pong_count = 0;

    if (s_gw_mode == GW_MODE_LONG_COMMAND) {
        /* One command that outlasts the node's drain window. Keep the connection
         * pinging so it stays healthy while the test stops the node underneath
         * the running command. */
        if (gw_send_invoke(TEST_SLOW_ID, TEST_COMMAND_LONG) != ESP_OK) {
            ESP_LOGE(TAG, "fake gateway: long invoke send failed");
        }
        /* s_gw_fd goes negative when the test tears the gateway down, which is
         * what stops this loop: a task left running past its test would
         * otherwise write onto whatever connection a later test had opened. */
        for (int i = 0; i < 100 && s_gw_fd >= 0; i++) {
            gw_send(HTTPD_WS_TYPE_PING, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(TEST_PING_INTERVAL_MS));
        }
        vTaskDelete(NULL);
        return;
    }

    if (s_gw_mode == GW_MODE_SPLIT_FRAME) {
        /* One invoke whose frame is written in two pieces, so the client hands
         * the node more than one event and the node has to stitch it back
         * together. */
        char *invoke = calloc(1, TEST_SPLIT_FRAME_LEN + 1);
        if (!invoke) {
            ESP_LOGE(TAG, "fake gateway: out of memory for the split frame");
            vTaskDelete(NULL);
            return;
        }
        snprintf(invoke, TEST_SPLIT_FRAME_LEN + 1,
                 "{\"type\":\"event\",\"event\":\"node.invoke.request\","
                 "\"payload\":{\"id\":\"%s\",\"command\":\"%s\",\"paramsJSON\":\"",
                 TEST_SPLIT_ID, TEST_COMMAND_ECHO);
        const size_t prefix = strlen(invoke);
        const size_t body = TEST_SPLIT_FRAME_LEN - 3;   /* room for the closing "}} */
        if (body > prefix) {
            memset(invoke + prefix, 'p', body - prefix);
            /* Terminate explicitly rather than relying on calloc having zeroed
             * the tail, which strlcat below would otherwise need. */
            invoke[body] = '\0';
        }
        strlcat(invoke, "\"}}", TEST_SPLIT_FRAME_LEN + 1);

        ESP_LOGI(TAG, "fake gateway: split frame of %u bytes", (unsigned)strlen(invoke));
        gw_send_text_frame_split(invoke, strlen(invoke), strlen(invoke) / 2, 250);
        free(invoke);

        for (int i = 0; i < 100 && s_gw.reply_count == 0 && s_gw_fd >= 0; i++) {
            gw_send(HTTPD_WS_TYPE_PING, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(TEST_PING_INTERVAL_MS));
        }
        vTaskDelete(NULL);
        return;
    }

    if (s_gw_mode == GW_MODE_OVERSIZED_FRAME) {
        /* A frame bigger than the node's receive buffer must be dropped without
         * disturbing the frames that follow it. */
        const size_t oversized = TEST_OVERSIZED_FRAME_LEN;
        char *big = malloc(oversized);
        if (!big) {
            ESP_LOGE(TAG, "fake gateway: out of memory for the oversized frame");
            vTaskDelete(NULL);
            return;
        }
        memset(big, 'x', oversized);
        ESP_LOGI(TAG, "fake gateway: oversized frame of %u bytes", (unsigned)oversized);
        gw_send(HTTPD_WS_TYPE_TEXT, big, oversized);
        free(big);

        vTaskDelay(pdMS_TO_TICKS(300));
        gw_send_invoke(TEST_SLOW_ID, TEST_COMMAND_ECHO);

        for (int i = 0; i < 100 && s_gw.reply_count == 0 && s_gw_fd >= 0; i++) {
            gw_send(HTTPD_WS_TYPE_PING, NULL, 0);
            vTaskDelay(pdMS_TO_TICKS(TEST_PING_INTERVAL_MS));
        }
        vTaskDelete(NULL);
        return;
    }

    if (gw_send_invoke(TEST_SLOW_ID, TEST_COMMAND_SLEEP) != ESP_OK) {
        ESP_LOGE(TAG, "fake gateway: slow invoke send failed");
        goto done;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gw_send_invoke(TEST_FAST_ID, TEST_COMMAND_ECHO) != ESP_OK) {
        ESP_LOGE(TAG, "fake gateway: fast invoke send failed");
        goto done;
    }

    for (int waited = 0; waited < TEST_SLOW_COMMAND_MS + 3000 && s_gw_fd >= 0;
         waited += TEST_PING_INTERVAL_MS) {
        gw_send(HTTPD_WS_TYPE_PING, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(TEST_PING_INTERVAL_MS));
        if (s_gw.reply_count >= 2) {
            break;
        }
    }
    ESP_LOGI(TAG, "fake gateway: %d pong(s) answered while the slow command ran",
             s_gw.pong_count);

done:
    vTaskDelete(NULL);
}

static void start_fake_gateway(void)
{
    memset(&s_gw, 0, sizeof(s_gw));
    s_gw_fd = -1;
    s_gw_events = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(s_gw_events);

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = TEST_GW_PORT;
    /* The node connects from this device over loopback, so no Wi-Fi is needed;
     * with if_name left NULL the server binds to any address. */
    TEST_ASSERT_EQUAL(ESP_OK, httpd_start(&s_gw_server, &httpd_config));

    /* handle_ws_control_frames is what makes the server hand PONGs to the
     * handler instead of swallowing them, and the liveness check needs them. */
    static const httpd_uri_t ws_uri = {
        .uri = TEST_GW_PATH,
        .method = HTTP_GET,
        .handler = gw_ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    TEST_ASSERT_EQUAL(ESP_OK, httpd_register_uri_handler(s_gw_server, &ws_uri));
}

static void stop_fake_gateway(void)
{
    if (s_gw_server) {
        httpd_stop(s_gw_server);
        s_gw_server = NULL;
    }
    if (s_gw_events) {
        vEventGroupDelete(s_gw_events);
        s_gw_events = NULL;
    }
    s_gw_fd = -1;
}

/* The test that would fail on the old inline dispatch: the command blocks the
 * WebSocket task for 2.5 s, so the client's receive loop stops and no PONG is
 * ever written, and the second request is not read until the first finishes.
 * With the worker in place the loop keeps running, the pings are answered
 * throughout, and both replies come back in the order the requests arrived. */
TEST_CASE("openclaw_node: a slow command does not stall the socket", "[openclaw_node]")
{
    s_gw_mode = GW_MODE_QUEUE_ORDER;
    start_fake_gateway();

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d%s", TEST_GW_PORT, TEST_GW_PATH);
    openclaw_node_config_t config = make_node_config(url);

    s_command_sleep_ms = TEST_SLOW_COMMAND_MS;
    s_command_calls = 0;

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(fake_gateway_task, "fake_gw", 6144, NULL, 5, NULL));

    const EventBits_t wanted = GW_SLOW_REPLY_BIT | GW_FAST_REPLY_BIT;
    EventBits_t bits = xEventGroupWaitBits(s_gw_events, wanted, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(TEST_SLOW_COMMAND_MS + 8000));

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());

    TEST_ASSERT_TRUE_MESSAGE((bits & GW_CONNECT_BIT) != 0,
                             "node never completed the connect handshake");
    TEST_ASSERT_TRUE_MESSAGE((bits & GW_SLOW_REPLY_BIT) != 0,
                             "no result arrived for the slow command");
    TEST_ASSERT_TRUE_MESSAGE((bits & GW_FAST_REPLY_BIT) != 0,
                             "no result arrived for the queued command");

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_gw.slow_reply, "\"ok\":true"),
                                 "the slow command result did not report success");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_gw.slow_reply, TEST_COMMAND_SLEEP),
                                 "the slow command result did not carry its output");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_gw.fast_reply, TEST_COMMAND_ECHO),
                                 "the queued command result did not carry its output");

    /* Requests are queued, so the slow one is answered first. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_gw.reply_count, "expected exactly two results");
    TEST_ASSERT_EQUAL_STRING(TEST_SLOW_ID, s_gw.reply_order[0]);
    TEST_ASSERT_EQUAL_STRING(TEST_FAST_ID, s_gw.reply_order[1]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_command_calls,
                                  "both commands should have run exactly once");

    /* The first ping goes out immediately and the command can finish early, so
     * require a handful rather than every ping in the window. A blocked
     * WebSocket task answers none of them. */
    TEST_ASSERT_TRUE_MESSAGE(s_gw.pong_count >= 4,
                             "the socket stopped answering pings while a command ran");

    ESP_LOGI(TAG, "slow command: %d pong(s) answered meanwhile", s_gw.pong_count);

    stop_fake_gateway();
}

/* ── http_server: the storage-root guard on delete ─────────────────────── */

/* The guard runs before the handler touches the filesystem, so these cases need
 * no mounted partition. A refused delete answers 400; an accepted one falls
 * through to stat() and answers 404 because nothing is mounted. That difference
 * is the assertion, and it is what proves the guard is actually wired in rather
 * than merely correct in isolation. */

static esp_err_t stub_load_config(app_config_t *config)
{
    memset(config, 0, sizeof(*config));
    app_config_load_defaults(config);
    return ESP_OK;
}

static esp_err_t stub_save_config(const app_config_t *config)
{
    (void)config;
    return ESP_OK;
}

static esp_err_t stub_get_wifi_status(http_server_wifi_status_t *status)
{
    memset(status, 0, sizeof(*status));
    return ESP_OK;
}

static esp_err_t stub_restart_device(void)
{
    return ESP_OK;
}

static int http_delete(const char *query, char *body, size_t body_size)
{
    char url[192];
    snprintf(url, sizeof(url), "http://127.0.0.1/api/files?%s", query);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return -1;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int read = esp_http_client_read_response(client, body, body_size - 1);
    body[read > 0 ? (size_t)read : 0] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static void start_http_server_for_test(void)
{
    const http_server_config_t config = {
        /* The refused cases never reach it, and it is absent for the rest. */
        .storage_base_path = "/no_such_storage",
        .services = {
            .load_config = stub_load_config,
            .save_config = stub_save_config,
            .get_wifi_status = stub_get_wifi_status,
            .restart_device = stub_restart_device,
        },
    };
    TEST_ASSERT_EQUAL(ESP_OK, http_server_init(&config));
    TEST_ASSERT_EQUAL(ESP_OK, http_server_start());
}

static void assert_delete_refused(const char *query)
{
    char body[256];
    int status = http_delete(query, body, sizeof(body));
    ESP_LOGI(TAG, "DELETE ?%s -> %d %s", query, status, body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(400, status, query);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, "storage root"),
                                 "expected the storage-root refusal");
}

static void assert_delete_allowed(const char *query)
{
    char body[256];
    int status = http_delete(query, body, sizeof(body));
    ESP_LOGI(TAG, "DELETE ?%s -> %d %s", query, status, body);
    TEST_ASSERT_EQUAL_INT_MESSAGE(404, status, query);
}

TEST_CASE("http_server: deleting the storage root is refused", "[http_server]")
{
    start_http_server_for_test();

    /* Every spelling the VFS resolves to the base directory. */
    assert_delete_refused("path=%2F&recursive=1");
    assert_delete_refused("path=%2F%2F&recursive=1");
    assert_delete_refused("path=%2F.&recursive=1");
    assert_delete_refused("path=%2F.%2F&recursive=1");
    assert_delete_refused("path=%2F.%2F%2F&recursive=1");

    TEST_ASSERT_EQUAL(ESP_OK, http_server_stop());
}

TEST_CASE("http_server: ordinary deletes are not mistaken for the root", "[http_server]")
{
    start_http_server_for_test();

    assert_delete_allowed("path=%2Fsomefile");
    assert_delete_allowed("path=%2F.foo");             /* a hidden name */
    assert_delete_allowed("path=%2F.%2Fsomefile");     /* a real child */
    assert_delete_allowed("path=%2Fdir%2F.");          /* "dir/." is not the root */

    TEST_ASSERT_EQUAL(ESP_OK, http_server_stop());
}

/* The hardest path in the worker teardown: a command that outlasts
 * NODE_CMD_DRAIN_WAIT_MS. stop() must give up on it rather than wait it out, and
 * it must not free the worker context underneath a worker that is still running
 * - it deliberately leaks it instead. So this asserts liveness, not heap
 * stability. The command keeps running to completion afterwards and its reply is
 * dropped, because s_cmd_worker no longer points at that worker. */
TEST_CASE("openclaw_node: stop() gives up on a command that outlasts the drain window",
          "[openclaw_node]")
{
    /* Comfortably longer than the node's own drain and exit windows (5 s each),
     * so both time out and the leak path is the one exercised. */
    enum { LONG_COMMAND_MS = 20000 };
    enum { DRAIN_WAIT_MS = 5000 };
    enum { EXIT_WAIT_MS = 5000 };

    s_gw_mode = GW_MODE_LONG_COMMAND;
    start_fake_gateway();

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d%s", TEST_GW_PORT, TEST_GW_PATH);
    openclaw_node_config_t config = make_node_config(url);

    s_command_sleep_ms = LONG_COMMAND_MS;
    s_long_command_started = 0;
    s_long_command_finished = 0;

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(fake_gateway_task, "fake_gw", 6144, NULL, 5, NULL));

    for (int i = 0; i < 100 && !s_long_command_started; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    TEST_ASSERT_TRUE_MESSAGE(s_long_command_started, "the long command never started");

    const int64_t started_us = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());
    const int elapsed_ms = (int)((esp_timer_get_time() - started_us) / 1000);

    ESP_LOGI(TAG, "stop() returned after %d ms with a %d ms command in flight",
             elapsed_ms, LONG_COMMAND_MS);

    /* It waited its drain window before giving up... */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(DRAIN_WAIT_MS - 1500, elapsed_ms,
                                         "stop() gave up before its drain window elapsed");
    /* ...but it did not wait the command out. */
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(LONG_COMMAND_MS - 1500, elapsed_ms,
                                      "stop() waited for the running command");

    /* The worker is left to finish on its own; the command is not abandoned
     * half-way, it simply has nobody left to answer. */
    for (int i = 0; i < 200 && !s_long_command_finished; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    TEST_ASSERT_TRUE_MESSAGE(s_long_command_finished,
                             "the command never ran to completion after stop()");

    s_gw_mode = GW_MODE_QUEUE_ORDER;
    stop_fake_gateway();
}

/* ── openclaw_node: a wss:// endpoint must actually start TLS ──────────── */
/* Without a CA source esp-tls refuses to build the TLS context at all - it logs
 * "No server verification option set in esp_tls_cfg_t structure" and returns
 * before the handshake - so nothing is ever written to the socket. With the
 * certificate bundle attached the handshake starts and the first byte on the
 * wire is a TLS handshake record (0x16). A plain TCP listener sees that
 * difference without needing a certificate of its own.
 *
 * Note that a TCP connection is made in both cases: esp_tls connects the socket
 * before it creates the TLS context, so "something connected" would not have
 * been a decisive assertion. */

#define TEST_TLS_PROBE_PORT 8092

typedef struct {
    volatile bool got_connection;
    volatile int  bytes_read;
    uint8_t       first_bytes[8];
} tls_probe_t;

static tls_probe_t s_probe;

static void tls_probe_task(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "tls probe: socket failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Bounded so the task cannot outlive the test when nothing connects. */
    struct timeval accept_timeout = { .tv_sec = 15, .tv_usec = 0 };
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TEST_TLS_PROBE_PORT),
    };
    if (bind(listen_fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "tls probe: bind/listen failed");
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    int fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0) {
        s_probe.got_connection = true;

        struct timeval read_timeout = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof(read_timeout));

        int n = recv(fd, s_probe.first_bytes, sizeof(s_probe.first_bytes), 0);
        if (n > 0) {
            s_probe.bytes_read = n;
        }
        close(fd);
    }
    close(listen_fd);
    vTaskDelete(NULL);
}

TEST_CASE("openclaw_node: a wss:// endpoint starts a TLS handshake", "[openclaw_node]")
{
    memset(&s_probe, 0, sizeof(s_probe));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(tls_probe_task, "tls_probe", 4096, NULL, 5, NULL));
    vTaskDelay(pdMS_TO_TICKS(200));

    char url[64];
    snprintf(url, sizeof(url), "wss://127.0.0.1:%d/ws", TEST_TLS_PROBE_PORT);
    openclaw_node_config_t config = make_node_config(url);

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));

    for (int i = 0; i < 100 && s_probe.bytes_read == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());

    const int bytes_read = s_probe.bytes_read;
    const uint8_t first_byte = s_probe.first_bytes[0];
    ESP_LOGI(TAG, "tls probe: connected=%d bytes=%d first=0x%02x",
             (int)s_probe.got_connection, bytes_read, first_byte);

    TEST_ASSERT_TRUE_MESSAGE(s_probe.got_connection,
                             "the node never reached the wss:// endpoint");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, bytes_read,
                                         "nothing was written, so TLS was never started");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x16, first_byte,
                                   "the first byte was not a TLS handshake record");
}

/* ── openclaw_node: frame reassembly and the oversized-frame guard ─────── */

/* The node rebuilds a frame from the events the client hands it, matching
 * payload_offset against its own byte count, and refuses anything that does not
 * fit its buffer. Both paths are buffer-safety critical, so they get their own
 * cases rather than being left to the round-trip tests above.
 *
 * The oversized case really does reach the node's guard: the client has no
 * buffer-size check of its own - it reads the frame in chunks and dispatches
 * each one with payload_len set to the frame's full length - so an oversized
 * frame is delivered rather than swallowed before the node sees it. */

TEST_CASE("openclaw_node: a frame split across reads is reassembled", "[openclaw_node]")
{
    s_gw_mode = GW_MODE_SPLIT_FRAME;
    start_fake_gateway();

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d%s", TEST_GW_PORT, TEST_GW_PATH);
    openclaw_node_config_t config = make_node_config(url);

    s_command_sleep_ms = 0;

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(fake_gateway_task, "fake_gw", 6144, NULL, 5, NULL));

    const EventBits_t bits = xEventGroupWaitBits(s_gw_events, GW_SPLIT_REPLY_BIT, pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(12000));

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());

    TEST_ASSERT_TRUE_MESSAGE((bits & GW_SPLIT_REPLY_BIT) != 0,
                             "the split frame was not reassembled into a usable request");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s_gw.split_reply, TEST_SPLIT_ID),
                                 "the reply did not carry the split request's id");

    s_gw_mode = GW_MODE_QUEUE_ORDER;
    stop_fake_gateway();
}

TEST_CASE("openclaw_node: an oversized frame is refused without wedging the connection",
          "[openclaw_node]")
{
    s_gw_mode = GW_MODE_OVERSIZED_FRAME;
    start_fake_gateway();

    char url[64];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d%s", TEST_GW_PORT, TEST_GW_PATH);
    openclaw_node_config_t config = make_node_config(url);

    s_command_sleep_ms = 0;
    s_command_calls = 0;

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_start(&config));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(fake_gateway_task, "fake_gw", 6144, NULL, 5, NULL));

    /* The invoke that follows the oversized frame still has to be answered. If
     * the guard had left the receive state wedged - a stale rx_length, say - it
     * would never be parsed. */
    const EventBits_t bits = xEventGroupWaitBits(s_gw_events, GW_SLOW_REPLY_BIT, pdFALSE, pdTRUE,
                                                 pdMS_TO_TICKS(12000));

    TEST_ASSERT_EQUAL(ESP_OK, openclaw_node_stop());

    TEST_ASSERT_TRUE_MESSAGE((bits & GW_SLOW_REPLY_BIT) != 0,
                             "the frame after the oversized one was never answered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_command_calls,
                                  "the command after the oversized frame did not run");

    s_gw_mode = GW_MODE_QUEUE_ORDER;
    stop_fake_gateway();
}

/* ── time_sync: waiting for a plausible clock ──────────────────────────── */

/* A wss:// handshake is only started once the clock is plausible, because a TLS
 * client validates the certificate dates against it. These cases pin both sides
 * of that wait: it gives up after its budget rather than blocking the caller,
 * and it returns at once when the clock is already good.
 *
 * time_sync_is_valid() is just "time(NULL) >= 2024-01-01", so settimeofday() is
 * enough to drive both paths without SNTP or a network. Each case sets the clock
 * itself, so the order they run in does not matter. */

TEST_CASE("time_sync: an unset clock times out instead of blocking", "[time_sync]")
{
    const struct timeval epoch = { .tv_sec = 0, .tv_usec = 0 };
    TEST_ASSERT_EQUAL(0, settimeofday(&epoch, NULL));
    TEST_ASSERT_FALSE(time_sync_is_valid());

    const int64_t started_us = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, time_sync_wait_valid(300));
    const int elapsed_ms = (int)((esp_timer_get_time() - started_us) / 1000);

    ESP_LOGI(TAG, "time_sync_wait_valid(300) returned after %d ms", elapsed_ms);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(250, elapsed_ms, "returned before its budget");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(1500, elapsed_ms, "waited past its budget");
}

TEST_CASE("time_sync: a plausible clock returns at once", "[time_sync]")
{
    const struct timeval future = { .tv_sec = 1900000000, .tv_usec = 0 };
    TEST_ASSERT_EQUAL(0, settimeofday(&future, NULL));
    TEST_ASSERT_TRUE(time_sync_is_valid());

    const int64_t started_us = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, time_sync_wait_valid(5000));
    const int elapsed_ms = (int)((esp_timer_get_time() - started_us) / 1000);

    TEST_ASSERT_LESS_THAN_INT_MESSAGE(200, elapsed_ms, "waited despite a valid clock");
}

void setUp(void)
{
    s_command_calls = 0;
}

void tearDown(void)
{
}

void app_main(void)
{
    ESP_LOGI(TAG, "fork regression tests");

    /* The fake Gateway is an HTTP server on this device, so the TCP/IP stack has
     * to be up even though the tests never associate with an AP. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    config_group_setup();
    unity_run_menu();
}
