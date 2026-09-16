/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-target regression tests for the fork's changes to the Native Node
 * transport.
 *
 * The transport must survive repeated start/stop without leaking, and must keep
 * servicing the socket while a slow command runs. That second property is the
 * point of running commands on a worker: the WebSocket task owns the receive
 * loop, so blocking it inside a command stalls the connection until the Gateway
 * gives up on the node.
 *
 * These tests need no Gateway. They run a WebSocket server on the device itself
 * and point the node at it over loopback, so they need neither Wi-Fi nor a
 * board. The evidence that the socket stays live is the PING/PONG traffic:
 * esp_transport_ws answers a PING from inside the client's receive loop, so a
 * WebSocket task that is stuck in a command answers none of them.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "openclaw_node.h"
#include "unity.h"
#include "unity_test_runner.h"

static const char *TAG = "fork_regression";

/* ── openclaw_node: the command callback used by the tests ─────────────── */

#define TEST_COMMAND_ECHO  "test.echo"
#define TEST_COMMAND_SLEEP "test.sleep"

static const char *s_test_commands[] = { TEST_COMMAND_ECHO, TEST_COMMAND_SLEEP };

static volatile int32_t s_command_sleep_ms;
static volatile int     s_command_calls;

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
#define TEST_SLOW_COMMAND_MS    2500
#define TEST_PING_INTERVAL_MS   200
#define TEST_TEXT_MAX           2048
#define TEST_MAX_REPLIES        4

#define GW_UPGRADED_BIT     BIT0
#define GW_CONNECT_BIT      BIT1
#define GW_SLOW_REPLY_BIT   BIT2
#define GW_FAST_REPLY_BIT   BIT3

typedef struct {
    int     pong_count;                 /* pongs seen since the reset */
    int     reply_count;
    char    reply_order[TEST_MAX_REPLIES][32];
    char    slow_reply[TEST_TEXT_MAX];
    char    fast_reply[TEST_TEXT_MAX];
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

/* Drives one scripted session: challenge, then a slow invoke with a fast one
 * right behind it, while pinging throughout. */
static void fake_gateway_task(void *arg)
{
    (void)arg;

    const char *challenge =
        "{\"type\":\"event\",\"event\":\"connect.challenge\","
        "\"payload\":{\"nonce\":\"test-nonce\",\"ts\":1700000000000}}";

    if ((xEventGroupWaitBits(s_gw_events, GW_UPGRADED_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(5000)) & GW_UPGRADED_BIT) == 0) {
        ESP_LOGE(TAG, "fake gateway: no client connected in time");
        goto done;
    }
    /* The node only sends its connect request once it has the challenge. */
    if (gw_send(HTTPD_WS_TYPE_TEXT, challenge, strlen(challenge)) != ESP_OK) {
        ESP_LOGE(TAG, "fake gateway: challenge send failed");
        goto done;
    }
    if ((xEventGroupWaitBits(s_gw_events, GW_CONNECT_BIT, pdFALSE, pdTRUE,
                             pdMS_TO_TICKS(5000)) & GW_CONNECT_BIT) == 0) {
        ESP_LOGE(TAG, "fake gateway: node never sent a connect request");
        goto done;
    }
    ESP_LOGI(TAG, "fake gateway: node connected");

    /* Measurement starts here: count pongs while the slow command runs, and put
     * the second request behind the first so the queue has to hold it. */
    s_gw.pong_count = 0;
    if (gw_send_invoke(TEST_SLOW_ID, TEST_COMMAND_SLEEP) != ESP_OK) {
        ESP_LOGE(TAG, "fake gateway: slow invoke send failed");
        goto done;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (gw_send_invoke(TEST_FAST_ID, TEST_COMMAND_ECHO) != ESP_OK) {
        ESP_LOGE(TAG, "fake gateway: fast invoke send failed");
        goto done;
    }

    for (int waited = 0; waited < TEST_SLOW_COMMAND_MS + 3000;
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

    unity_run_menu();
}
