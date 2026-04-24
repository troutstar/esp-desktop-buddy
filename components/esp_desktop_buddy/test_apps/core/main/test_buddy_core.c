/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "unity.h"

#include "esp_desktop_buddy/core.h"
#include "esp_desktop_buddy/transport_port.h"
#include "esp_desktop_buddy/folder_push.h"
#include "esp_desktop_buddy/snapshot.h"

#define BUDDY_ENTRY_COUNT_MAX CONFIG_ESP_DESKTOP_BUDDY_ENTRY_COUNT_MAX
#define BUDDY_ENTRY_STRING_MAX CONFIG_ESP_DESKTOP_BUDDY_ENTRY_STRING_MAX
#define BUDDY_MESSAGE_MAX CONFIG_ESP_DESKTOP_BUDDY_MESSAGE_MAX
#define BUDDY_PROMPT_ID_MAX CONFIG_ESP_DESKTOP_BUDDY_PROMPT_ID_MAX
#define BUDDY_PROMPT_TOOL_MAX CONFIG_ESP_DESKTOP_BUDDY_PROMPT_TOOL_MAX
#define BUDDY_PROMPT_HINT_MAX CONFIG_ESP_DESKTOP_BUDDY_PROMPT_HINT_MAX

typedef struct {
    bool present;
    char id[BUDDY_PROMPT_ID_MAX + 1];
    char tool[BUDDY_PROMPT_TOOL_MAX + 1];
    char hint[BUDDY_PROMPT_HINT_MAX + 1];
} buddy_prompt_t;

typedef struct {
    bool has_state;
    uint32_t total;
    uint32_t running;
    uint32_t waiting;
    uint64_t tokens;
    uint64_t tokens_today;
    char msg[BUDDY_MESSAGE_MAX + 1];
    char entries[BUDDY_ENTRY_COUNT_MAX][BUDDY_ENTRY_STRING_MAX + 1];
    size_t entry_count;
    buddy_prompt_t prompt;
} buddy_snapshot_t;

typedef struct {
    SemaphoreHandle_t mutex;
    esp_desktop_buddy_t *buddy;
    size_t snapshot_count;
    size_t prompt_response_count;
    size_t turn_count;
    size_t time_sync_count;
    size_t live_change_count;
    size_t error_count;
    bool last_live;
    buddy_snapshot_t last_snapshot;
    esp_desktop_buddy_permission_sent_t last_prompt_response;
    char last_prompt_id[BUDDY_PROMPT_ID_MAX + 1];
    esp_desktop_buddy_time_sync_t last_time_sync;
    esp_desktop_buddy_error_event_t last_error;
    char last_turn_role[ESP_DESKTOP_BUDDY_TURN_ROLE_MAX + 1];
    char last_turn_content[ESP_DESKTOP_BUDDY_LINE_MAX + 1];
} esp_desktop_buddy_test_events_t;

typedef struct {
    size_t begin_transfer_count;
    size_t begin_file_count;
    size_t write_chunk_count;
    size_t end_file_count;
    size_t end_transfer_count;
    size_t abort_count;
    char last_transfer_name[65];
    char last_path[65];
    uint32_t last_total_bytes;
    uint32_t last_file_size;
    uint8_t last_bytes[64];
    size_t last_bytes_len;
} buddy_push_mock_t;

typedef struct {
    size_t name_count;
    size_t owner_count;
    size_t unpair_count;
    const uint8_t *status_bytes;
    size_t status_len;
    bool status_should_fail;
    esp_err_t status_err;
    const char *status_detail;
    char last_name[BUDDY_MESSAGE_MAX + 1];
    char last_owner[BUDDY_MESSAGE_MAX + 1];
} buddy_handler_test_ctx_t;

typedef struct {
    bool consumed;
    bool saw_invalid_arg;
} buddy_extension_test_ctx_t;

typedef struct {
    SemaphoreHandle_t done;
    esp_desktop_buddy_t *buddy;
    size_t snapshot_count;
    esp_err_t delete_result;
} esp_desktop_buddy_delete_on_event_ctx_t;

static esp_desktop_buddy_test_events_t s_events;
static char s_tx_line[ESP_DESKTOP_BUDDY_FRAME_MAX + 1];

static void esp_desktop_buddy_test_snapshot_reset(buddy_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
}

static esp_err_t esp_desktop_buddy_test_capture_snapshot(esp_desktop_buddy_t *buddy, buddy_snapshot_t *snapshot)
{
    esp_desktop_buddy_snapshot_info_t info = {0};
    size_t entry_count = 0;
    esp_err_t err;

    if (buddy == NULL || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_desktop_buddy_test_snapshot_reset(snapshot);

    err = esp_desktop_buddy_get_snapshot_info(buddy, &info);
    if (err != ESP_OK) {
        return err;
    }

    snapshot->has_state = info.has_state;
    snapshot->total = info.total;
    snapshot->running = info.running;
    snapshot->waiting = info.waiting;
    snapshot->tokens = info.tokens;
    snapshot->tokens_today = info.tokens_today;
    snapshot->prompt.present = info.prompt_present;

    if (!info.has_state) {
        return ESP_OK;
    }

    err = esp_desktop_buddy_get_message(buddy, snapshot->msg, sizeof(snapshot->msg), NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_desktop_buddy_get_entry_count(buddy, &entry_count);
    if (err != ESP_OK) {
        return err;
    }
    if (entry_count > BUDDY_ENTRY_COUNT_MAX) {
        entry_count = BUDDY_ENTRY_COUNT_MAX;
    }
    snapshot->entry_count = entry_count;
    for (size_t i = 0; i < entry_count; ++i) {
        err = esp_desktop_buddy_get_entry(buddy, i, snapshot->entries[i], sizeof(snapshot->entries[i]), NULL);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (!info.prompt_present) {
        return ESP_OK;
    }

    err = esp_desktop_buddy_get_prompt_id(buddy, snapshot->prompt.id, sizeof(snapshot->prompt.id), NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_desktop_buddy_get_prompt_tool(buddy, snapshot->prompt.tool, sizeof(snapshot->prompt.tool), NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_desktop_buddy_get_prompt_hint(buddy, snapshot->prompt.hint, sizeof(snapshot->prompt.hint), NULL);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

static void esp_desktop_buddy_test_reset_events(void)
{
    xSemaphoreTake(s_events.mutex, portMAX_DELAY);
    memset(&s_events.snapshot_count, 0, sizeof(s_events) - offsetof(esp_desktop_buddy_test_events_t, snapshot_count));
    xSemaphoreGive(s_events.mutex);
}

static void esp_desktop_buddy_test_on_event(void *ctx, const esp_desktop_buddy_event_t *event)
{
    esp_desktop_buddy_test_events_t *events = (esp_desktop_buddy_test_events_t *)ctx;

    xSemaphoreTake(events->mutex, portMAX_DELAY);
    switch (event->type) {
    case ESP_DESKTOP_BUDDY_EVENT_SNAPSHOT_UPDATED:
        events->snapshot_count++;
        if (events->buddy != NULL) {
            (void)esp_desktop_buddy_test_capture_snapshot(events->buddy, &events->last_snapshot);
        } else {
            esp_desktop_buddy_test_snapshot_reset(&events->last_snapshot);
        }
        break;
    case ESP_DESKTOP_BUDDY_EVENT_PERMISSION_SENT:
        events->prompt_response_count++;
        events->last_prompt_response.decision = event->data.permission_sent.decision;
        strlcpy(events->last_prompt_id,
                event->data.permission_sent.prompt_id != NULL ? event->data.permission_sent.prompt_id : "",
                sizeof(events->last_prompt_id));
        events->last_prompt_response.prompt_id = events->last_prompt_id;
        break;
    case ESP_DESKTOP_BUDDY_EVENT_TURN: {
        size_t copy_len = event->data.turn.content.len;

        events->turn_count++;
        strlcpy(events->last_turn_role,
                event->data.turn.role,
                sizeof(events->last_turn_role));
        memset(events->last_turn_content, 0, sizeof(events->last_turn_content));
        if (copy_len >= sizeof(events->last_turn_content)) {
            copy_len = sizeof(events->last_turn_content) - 1;
        }
        memcpy(events->last_turn_content, event->data.turn.content.bytes, copy_len);
        break;
    }
    case ESP_DESKTOP_BUDDY_EVENT_TIME_SYNC:
        events->time_sync_count++;
        events->last_time_sync = event->data.time_sync;
        break;
    case ESP_DESKTOP_BUDDY_EVENT_LIVENESS_CHANGED:
        events->live_change_count++;
        events->last_live = event->data.live;
        break;
    case ESP_DESKTOP_BUDDY_EVENT_ERROR:
        events->error_count++;
        events->last_error = event->data.error;
        break;
    default:
        break;
    }
    xSemaphoreGive(events->mutex);
}

static esp_desktop_buddy_status_reply_t esp_desktop_buddy_test_invalid_status_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    static const uint8_t invalid_status[] = "[]";

    (void)ctx;
    (void)buddy;
    return esp_desktop_buddy_status_ok((esp_desktop_buddy_json_object_view_t){
        .bytes = invalid_status,
        .len = sizeof(invalid_status) - 1,
    });
}

static esp_desktop_buddy_status_reply_t esp_desktop_buddy_test_status_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    buddy_handler_test_ctx_t *handler_ctx = (buddy_handler_test_ctx_t *)ctx;

    (void)buddy;
    if (handler_ctx->status_should_fail) {
        return esp_desktop_buddy_status_err(handler_ctx->status_err, handler_ctx->status_detail);
    }
    return esp_desktop_buddy_status_ok((esp_desktop_buddy_json_object_view_t){
        .bytes = handler_ctx->status_bytes,
        .len = handler_ctx->status_len,
    });
}

static esp_desktop_buddy_command_result_t esp_desktop_buddy_test_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    buddy_handler_test_ctx_t *handler_ctx = (buddy_handler_test_ctx_t *)ctx;

    (void)buddy;
    handler_ctx->name_count++;
    strlcpy(handler_ctx->last_name, name != NULL ? name : "", sizeof(handler_ctx->last_name));
    return esp_desktop_buddy_command_ok();
}

static esp_desktop_buddy_command_result_t esp_desktop_buddy_test_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    buddy_handler_test_ctx_t *handler_ctx = (buddy_handler_test_ctx_t *)ctx;

    (void)buddy;
    handler_ctx->owner_count++;
    strlcpy(handler_ctx->last_owner, name != NULL ? name : "", sizeof(handler_ctx->last_owner));
    return esp_desktop_buddy_command_err(ESP_ERR_NOT_ALLOWED, "owner_rejected");
}

static esp_desktop_buddy_command_result_t esp_desktop_buddy_test_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    buddy_handler_test_ctx_t *handler_ctx = (buddy_handler_test_ctx_t *)ctx;

    (void)buddy;
    handler_ctx->unpair_count++;
    return esp_desktop_buddy_command_ok();
}

static esp_desktop_buddy_command_extension_result_t esp_desktop_buddy_test_no_ack_extension(
    void *ctx,
    esp_desktop_buddy_t *buddy,
    const esp_desktop_buddy_command_view_t *request)
{
    buddy_extension_test_ctx_t *extension_ctx = (buddy_extension_test_ctx_t *)ctx;

    (void)buddy;
    (void)request;

    if (extension_ctx != NULL) {
        extension_ctx->consumed = true;
    }

    return (esp_desktop_buddy_command_extension_result_t){
        .mode = ESP_DESKTOP_BUDDY_COMMAND_EXTENSION_NO_ACK,
    };
}

static esp_desktop_buddy_command_extension_result_t esp_desktop_buddy_test_string_arg_guard_extension(
    void *ctx,
    esp_desktop_buddy_t *buddy,
    const esp_desktop_buddy_command_view_t *request)
{
    buddy_extension_test_ctx_t *extension_ctx = (buddy_extension_test_ctx_t *)ctx;

    (void)buddy;

    if (extension_ctx != NULL) {
        extension_ctx->saw_invalid_arg =
            esp_desktop_buddy_command_view_get_string(request, "name", NULL) == ESP_ERR_INVALID_ARG;
    }

    return (esp_desktop_buddy_command_extension_result_t){
        .mode = ESP_DESKTOP_BUDDY_COMMAND_EXTENSION_ACK,
        .reply = {
            .ok = true,
            .n = 0,
            .error = NULL,
        },
    };
}

static esp_desktop_buddy_folder_push_sink_result_t buddy_push_mock_begin_transfer(void *ctx,
                                                                 const char *name,
                                                                 uint32_t total_bytes)
{
    buddy_push_mock_t *mock = (buddy_push_mock_t *)ctx;

    mock->begin_transfer_count++;
    mock->last_total_bytes = total_bytes;
    strlcpy(mock->last_transfer_name, name, sizeof(mock->last_transfer_name));
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t buddy_push_mock_begin_file(void *ctx,
                                                             const char *path,
                                                             uint32_t size)
{
    buddy_push_mock_t *mock = (buddy_push_mock_t *)ctx;

    mock->begin_file_count++;
    mock->last_file_size = size;
    strlcpy(mock->last_path, path, sizeof(mock->last_path));
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t buddy_push_mock_write_chunk(void *ctx,
                                                              const uint8_t *data,
                                                              size_t len)
{
    buddy_push_mock_t *mock = (buddy_push_mock_t *)ctx;

    mock->write_chunk_count++;
    if (len > sizeof(mock->last_bytes)) {
        return esp_desktop_buddy_folder_push_result_err(ESP_FAIL,
                                                        ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED,
                                                        "too_large");
    }
    mock->last_bytes_len = len;
    memcpy(mock->last_bytes, data, len);
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t buddy_push_mock_end_file(void *ctx)
{
    buddy_push_mock_t *mock = (buddy_push_mock_t *)ctx;

    mock->end_file_count++;
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t buddy_push_mock_end_transfer(void *ctx)
{
    buddy_push_mock_t *mock = (buddy_push_mock_t *)ctx;

    mock->end_transfer_count++;
    return esp_desktop_buddy_folder_push_result_ok();
}

static void buddy_push_mock_abort(void *ctx)
{
    buddy_push_mock_t *mock = (buddy_push_mock_t *)ctx;

    mock->abort_count++;
}

static esp_err_t esp_desktop_buddy_test_new_push(buddy_push_mock_t *mock, esp_desktop_buddy_folder_push_t **out_push)
{
    esp_desktop_buddy_folder_push_config_t config = {
        .sink = {
            .begin_transfer = buddy_push_mock_begin_transfer,
            .begin_file = buddy_push_mock_begin_file,
            .write_chunk = buddy_push_mock_write_chunk,
            .end_file = buddy_push_mock_end_file,
            .end_transfer = buddy_push_mock_end_transfer,
            .abort_transfer = buddy_push_mock_abort,
            .ctx = mock,
        },
    };

    memset(mock, 0, sizeof(*mock));
    return esp_desktop_buddy_folder_push_new(&config, out_push);
}

static esp_desktop_buddy_t *esp_desktop_buddy_test_new(const esp_desktop_buddy_command_handlers_t *handlers)
{
    esp_desktop_buddy_t *buddy = NULL;
    esp_desktop_buddy_config_t config = {
        .event_sink = {
            .on_event = esp_desktop_buddy_test_on_event,
            .ctx = &s_events,
        },
    };

    if (handlers != NULL) {
        config.handlers = *handlers;
    }

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_new(&config, &buddy));
    s_events.buddy = buddy;
    return buddy;
}

static esp_desktop_buddy_t *esp_desktop_buddy_test_new_with_event_sink(const esp_desktop_buddy_command_handlers_t *handlers,
                                               esp_desktop_buddy_event_listener_t event_sink)
{
    esp_desktop_buddy_t *buddy = NULL;
    esp_desktop_buddy_config_t config = {
        .event_sink = event_sink,
    };

    if (handlers != NULL) {
        config.handlers = *handlers;
    }

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_new(&config, &buddy));
    return buddy;
}

static void esp_desktop_buddy_test_delete(esp_desktop_buddy_t *buddy)
{
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_delete(buddy));
    s_events.buddy = NULL;
}

static void esp_desktop_buddy_test_wait(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void esp_desktop_buddy_test_feed_line(esp_desktop_buddy_t *buddy, const char *line)
{
    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_desktop_buddy_receive_bytes(buddy,
                                          (const uint8_t *)line,
                                          strlen(line)));
}

static void esp_desktop_buddy_test_feed_bytes(esp_desktop_buddy_t *buddy, const uint8_t *bytes, size_t len)
{
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_receive_bytes(buddy, bytes, len));
}

static void esp_desktop_buddy_test_fill_inbox_until_full(esp_desktop_buddy_t *buddy)
{
    static const char *line = "{\"time\":[0,0]}\n";
    size_t inbox_depth = CONFIG_ESP_DESKTOP_BUDDY_RX_INBOX_DEPTH + CONFIG_ESP_DESKTOP_BUDDY_ACTION_QUEUE_DEPTH;

    vTaskSuspendAll();
    for (size_t i = 0; i < inbox_depth; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          esp_desktop_buddy_receive_bytes(buddy, (const uint8_t *)line, strlen(line)));
    }
}

static void esp_desktop_buddy_test_read_tx_line(esp_desktop_buddy_t *buddy, char *out, size_t out_size)
{
    size_t len = 0;

    memset(out, 0, out_size);
    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_desktop_buddy_transport_port_next_frame(
                          buddy, (uint8_t *)out, out_size - 1, &len, pdMS_TO_TICKS(200)));
    TEST_ASSERT_GREATER_THAN_UINT32(0, len);
    if (len > 0 && out[len - 1] == '\n') {
        out[len - 1] = '\0';
    } else {
        out[len] = '\0';
    }
}

static void esp_desktop_buddy_test_assert_no_tx(esp_desktop_buddy_t *buddy)
{
    uint8_t byte = 0;
    size_t len = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      esp_desktop_buddy_transport_port_next_frame(buddy, &byte, sizeof(byte), &len, pdMS_TO_TICKS(20)));
}

static void test_tx_dequeue_with_small_buffer_preserves_frame(void)
{
    static const char *prompt_snapshot =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":1,\"tokens_today\":2,"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    uint8_t byte = 0;
    size_t len = 0;

    esp_desktop_buddy_test_feed_line(buddy, prompt_snapshot);
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_approve_once(buddy, "p1"));
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      esp_desktop_buddy_transport_port_next_frame(buddy, &byte, sizeof(byte), &len, pdMS_TO_TICKS(200)));
    TEST_ASSERT_GREATER_THAN_UINT32(1, len);

    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"cmd\":\"permission\",\"id\":\"p1\",\"decision\":\"once\"}", s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_delete_refuses_attached_transport(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_transport_port_attach(buddy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, esp_desktop_buddy_delete(buddy));

    esp_desktop_buddy_transport_port_detach(buddy);
    esp_desktop_buddy_test_delete(buddy);
}

static void test_snapshot_entries_skip_invalid_items_and_prompt_is_retained(void)
{
    static const char *part1 =
        "{\"total\":2,\"running\":1,\"waiting\":1,\"msg\":\"approve\",";
    static const char *part2 =
        "\"tokens\":5,\"tokens_today\":9,\"entries\":[\"alpha\",42],"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t snapshot = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, part1);
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL_UINT32(0, s_events.snapshot_count);

    esp_desktop_buddy_test_feed_line(buddy, part2);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.snapshot_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.total);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.entry_count);
    TEST_ASSERT_EQUAL_STRING("alpha", snapshot.entries[0]);
    TEST_ASSERT_TRUE(snapshot.prompt.present);
    TEST_ASSERT_EQUAL_STRING("p1", snapshot.prompt.id);
    TEST_ASSERT_EQUAL_STRING("Bash", snapshot.prompt.tool);
    TEST_ASSERT_EQUAL_STRING("ls", snapshot.prompt.hint);
    TEST_ASSERT_EQUAL_STRING("approve", snapshot.msg);

    esp_desktop_buddy_delete(buddy);
}

static void test_snapshot_replacement_clears_optional_fields(void)
{
    static const char *first_snapshot =
        "{\"total\":3,\"running\":2,\"waiting\":1,\"msg\":\"first\","
        "\"tokens\":10,\"tokens_today\":20,\"entries\":[\"alpha\"],"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"},"
        "\"extra\":true}\n";
    static const char *second_snapshot =
        "{\"total\":4,\"running\":1,\"waiting\":3,\"msg\":\"second\","
        "\"tokens\":30,\"tokens_today\":40}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t snapshot = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, first_snapshot);
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_feed_line(buddy, second_snapshot);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(2, s_events.snapshot_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(4, snapshot.total);
    TEST_ASSERT_EQUAL_UINT32(1, snapshot.running);
    TEST_ASSERT_EQUAL_UINT32(3, snapshot.waiting);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.entry_count);
    TEST_ASSERT_FALSE(snapshot.prompt.present);
    TEST_ASSERT_EQUAL_STRING("second", snapshot.msg);
    TEST_ASSERT_TRUE_MESSAGE(snapshot.tokens == 30ULL, "unexpected snapshot.tokens");
    TEST_ASSERT_TRUE_MESSAGE(snapshot.tokens_today == 40ULL,
                             "unexpected snapshot.tokens_today");

    esp_desktop_buddy_delete(buddy);
}

static void test_overlong_prompt_id_rejects_snapshot(void)
{
    static const char *good_snapshot =
        "{\"total\":1,\"running\":1,\"waiting\":0,\"msg\":\"good\","
        "\"tokens\":7,\"tokens_today\":8}\n";
    static const char *bad_snapshot =
        "{\"total\":2,\"running\":1,\"waiting\":1,\"msg\":\"bad\","
        "\"tokens\":9,\"tokens_today\":10,"
        "\"prompt\":{\"id\":\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNO\","
        "\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t before = {0};
    buddy_snapshot_t after = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, good_snapshot);
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &before));

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, bad_snapshot);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(0, s_events.snapshot_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));

    esp_desktop_buddy_delete(buddy);
}

static void test_malformed_prompt_degrades_to_absent(void)
{
    static const char *snapshot_line =
        "{\"total\":1,\"running\":0,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":5,\"tokens_today\":6,"
        "\"prompt\":{\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t snapshot = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, snapshot_line);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.snapshot_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &snapshot));
    TEST_ASSERT_FALSE(snapshot.prompt.present);

    esp_desktop_buddy_delete(buddy);
}

static void test_liveness_timeout_keeps_last_snapshot(void)
{
    static const char *snapshot_line =
        "{\"total\":1,\"running\":1,\"waiting\":0,\"msg\":\"idle\","
        "\"tokens\":7,\"tokens_today\":8}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t before = {0};
    buddy_snapshot_t after = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, snapshot_line);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_TRUE(esp_desktop_buddy_is_live(buddy));
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &before));

    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESP_DESKTOP_BUDDY_LIVE_TIMEOUT_MS + 200));

    TEST_ASSERT_FALSE(esp_desktop_buddy_is_live(buddy));
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, s_events.live_change_count);
    TEST_ASSERT_FALSE(s_events.last_live);

    esp_desktop_buddy_delete(buddy);
}

static void test_malformed_required_snapshot_is_rejected(void)
{
    static const char *good_snapshot =
        "{\"total\":1,\"running\":1,\"waiting\":0,\"msg\":\"good\","
        "\"tokens\":7,\"tokens_today\":8}\n";
    static const char *bad_snapshot =
        "{\"total\":\"oops\",\"running\":2,\"waiting\":0,\"msg\":\"bad\","
        "\"tokens\":9,\"tokens_today\":10}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t before = {0};
    buddy_snapshot_t after = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, good_snapshot);
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &before));

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, bad_snapshot);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(0, s_events.snapshot_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &after));
    TEST_ASSERT_EQUAL_MEMORY(&before, &after, sizeof(before));

    esp_desktop_buddy_delete(buddy);
}

static void test_snapshot_token_values_are_clamped_and_truncated(void)
{
    static const char *snapshot_line =
        "{\"total\":1,\"running\":0,\"waiting\":1,\"msg\":\"big\","
        "\"tokens\":18446744073709551615,\"tokens_today\":1.75}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t snapshot = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, snapshot_line);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.snapshot_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &snapshot));
    TEST_ASSERT_TRUE_MESSAGE(snapshot.tokens == UINT64_MAX, "unexpected snapshot.tokens");
    TEST_ASSERT_TRUE_MESSAGE(snapshot.tokens_today == 1ULL, "unexpected snapshot.tokens_today");

    esp_desktop_buddy_delete(buddy);
}

static void test_snapshot_counter_values_are_clamped_and_truncated(void)
{
    static const char *snapshot_line =
        "{\"total\":-1,\"running\":2.75,\"waiting\":1e30,\"msg\":\"bad\","
        "\"tokens\":1,\"tokens_today\":10}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t snapshot = {0};

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, snapshot_line);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.snapshot_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.total);
    TEST_ASSERT_EQUAL_UINT32(2, snapshot.running);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, snapshot.waiting);

    esp_desktop_buddy_delete(buddy);
}

static void test_time_sync_values_are_clamped(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "{\"time\":[9223372036854775808,-1e30]}\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.time_sync_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.error_count);
    TEST_ASSERT_TRUE_MESSAGE(s_events.last_time_sync.epoch == INT64_MAX,
                             "unexpected time sync epoch");
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, s_events.last_time_sync.tz_offset);

    esp_desktop_buddy_delete(buddy);
}

static void test_time_sync_fractional_values_are_truncated(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "{\"time\":[1713571200.75,19800.9]}\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.time_sync_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.error_count);
    TEST_ASSERT_TRUE_MESSAGE(s_events.last_time_sync.epoch == 1713571200LL,
                             "unexpected time sync epoch");
    TEST_ASSERT_EQUAL_INT32(19800, s_events.last_time_sync.tz_offset);

    esp_desktop_buddy_delete(buddy);
}

static void test_permission_reply_is_encoded_and_emitted(void)
{
    static const char *prompt_snapshot =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":1,\"tokens_today\":2,"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, prompt_snapshot);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_approve_once(buddy, "p1"));
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"cmd\":\"permission\",\"id\":\"p1\",\"decision\":\"once\"}", s_tx_line);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.prompt_response_count);
    TEST_ASSERT_EQUAL_STRING("p1", s_events.last_prompt_response.prompt_id);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_PERMISSION_DECISION_ONCE,
                      s_events.last_prompt_response.decision);

    esp_desktop_buddy_delete(buddy);
}

static void test_same_prompt_snapshot_allows_permission_retry(void)
{
    static const char *prompt_snapshot =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":1,\"tokens_today\":2,"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, prompt_snapshot);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_approve_once(buddy, "p1"));
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"cmd\":\"permission\",\"id\":\"p1\",\"decision\":\"once\"}", s_tx_line);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, prompt_snapshot);
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_deny(buddy, "p1"));
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"cmd\":\"permission\",\"id\":\"p1\",\"decision\":\"deny\"}", s_tx_line);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.prompt_response_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.error_count);

    esp_desktop_buddy_delete(buddy);
}

static void test_stale_prompt_action_is_rejected_without_tx(void)
{
    static const char *prompt_one =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":1,\"tokens_today\":2,"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    static const char *prompt_two =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":3,\"tokens_today\":4,"
        "\"prompt\":{\"id\":\"p2\",\"tool\":\"Edit\",\"hint\":\"fix\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_feed_line(buddy, prompt_one);
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_feed_line(buddy, prompt_two);
    esp_desktop_buddy_test_wait();

    esp_desktop_buddy_test_reset_events();
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_approve_once(buddy, "p1"));
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL_UINT32(0, s_events.prompt_response_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_ACTION, s_events.last_error.kind);
    esp_desktop_buddy_test_assert_no_tx(buddy);

    esp_desktop_buddy_test_reset_events();
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_deny(buddy, "p2"));
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL_UINT32(1, s_events.prompt_response_count);
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"cmd\":\"permission\",\"id\":\"p2\",\"decision\":\"deny\"}", s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_expired_prompt_action_is_rejected_without_tx(void)
{
    static const char *prompt_snapshot =
        "{\"total\":1,\"running\":1,\"waiting\":1,\"msg\":\"approve\","
        "\"tokens\":1,\"tokens_today\":2,"
        "\"prompt\":{\"id\":\"p1\",\"tool\":\"Bash\",\"hint\":\"ls\"}}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_feed_line(buddy, prompt_snapshot);
    esp_desktop_buddy_test_wait();
    vTaskDelay(pdMS_TO_TICKS(CONFIG_ESP_DESKTOP_BUDDY_LIVE_TIMEOUT_MS + 100));
    esp_desktop_buddy_test_wait();

    esp_desktop_buddy_test_reset_events();
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_prompt_approve_once(buddy, "p1"));
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_FALSE(esp_desktop_buddy_is_live(buddy));
    TEST_ASSERT_EQUAL_UINT32(0, s_events.prompt_response_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_ACTION, s_events.last_error.kind);
    esp_desktop_buddy_test_assert_no_tx(buddy);

    esp_desktop_buddy_delete(buddy);
}

static void test_post_action_rejects_invalid_type(void)
{
    esp_desktop_buddy_permission_reply_t action = {0};
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    action.type = (esp_desktop_buddy_permission_reply_type_t)99;
    action.prompt_id = "p1";

    esp_desktop_buddy_test_reset_events();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_desktop_buddy_post_permission_reply(buddy, &action));
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL_UINT32(0, s_events.error_count);
    esp_desktop_buddy_test_assert_no_tx(buddy);

    esp_desktop_buddy_delete(buddy);
}

static void test_invalid_status_data_falls_back_to_negative_ack(void)
{
    esp_desktop_buddy_command_handlers_t handlers = {
        .ctx = NULL,
        .on_status = esp_desktop_buddy_test_invalid_status_handler,
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"status\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"status\",\"ok\":false,\"n\":0,\"error\":\"invalid_status_data\"}",
        s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_status_handler_errors_are_mapped_to_stable_tokens(void)
{
    buddy_handler_test_ctx_t handler_ctx = {
        .status_should_fail = true,
        .status_err = ESP_ERR_NOT_ALLOWED,
        .status_detail = "busy",
    };
    esp_desktop_buddy_command_handlers_t handlers = {
        .ctx = &handler_ctx,
        .on_status = esp_desktop_buddy_test_status_handler,
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"status\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"status\",\"ok\":false,\"n\":0,\"error\":\"rejected\"}",
        s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_status_too_large_falls_back_to_negative_ack(void)
{
    size_t blob_len = ESP_DESKTOP_BUDDY_LINE_MAX;
    size_t json_cap = blob_len + 32;
    char *json = malloc(json_cap);
    buddy_handler_test_ctx_t handler_ctx = {0};
    esp_desktop_buddy_command_handlers_t handlers = {
        .ctx = &handler_ctx,
        .on_status = esp_desktop_buddy_test_status_handler,
    };
    esp_desktop_buddy_t *buddy;

    TEST_ASSERT_NOT_NULL(json);
    memcpy(json, "{\"blob\":\"", 9);
    memset(json + 9, 'a', blob_len);
    memcpy(json + 9 + blob_len, "\"}", 3);

    handler_ctx.status_bytes = (const uint8_t *)json;
    handler_ctx.status_len = strlen(json);
    TEST_ASSERT_TRUE(handler_ctx.status_len > ESP_DESKTOP_BUDDY_LINE_MAX);

    buddy = esp_desktop_buddy_test_new(&handlers);
    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"status\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"status\",\"ok\":false,\"n\":0,\"error\":\"status_too_large\"}",
        s_tx_line);

    esp_desktop_buddy_delete(buddy);
    free(json);
}

static void test_unknown_command_is_nacked(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"mystery\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"mystery\",\"ok\":false,\"n\":0,\"error\":\"unknown_command\"}",
        s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_command_extension_can_consume_without_ack(void)
{
    buddy_extension_test_ctx_t extension_ctx = {0};
    static const esp_desktop_buddy_command_extension_entry_t bindings[] = {
        { .command = "char_begin", .handler = esp_desktop_buddy_test_no_ack_extension },
    };
    esp_desktop_buddy_command_handlers_t handlers = {
        .ctx = &extension_ctx,
        .command_extension = {
            .ctx = &extension_ctx,
            .bindings = bindings,
            .binding_count = sizeof(bindings) / sizeof(bindings[0]),
        },
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"char_begin\",\"name\":\"demo\",\"total\":3}\n");
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_TRUE(extension_ctx.consumed);
    esp_desktop_buddy_test_assert_no_tx(buddy);

    esp_desktop_buddy_delete(buddy);
}

static void test_command_request_get_string_rejects_null_output(void)
{
    buddy_extension_test_ctx_t extension_ctx = {0};
    static const esp_desktop_buddy_command_extension_entry_t bindings[] = {
        { .command = "probe", .handler = esp_desktop_buddy_test_string_arg_guard_extension },
    };
    esp_desktop_buddy_command_handlers_t handlers = {
        .command_extension = {
            .ctx = &extension_ctx,
            .bindings = bindings,
            .binding_count = sizeof(bindings) / sizeof(bindings[0]),
        },
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"probe\",\"name\":\"demo\"}\n");
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_TRUE(extension_ctx.saw_invalid_arg);
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"probe\",\"ok\":true,\"n\":0}", s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_unsupported_commands_are_nacked(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"name\",\"name\":\"Desk\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"name\",\"ok\":false,\"n\":0,\"error\":\"unsupported\"}",
        s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"owner\",\"name\":\"Felix\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"owner\",\"ok\":false,\"n\":0,\"error\":\"unsupported\"}",
        s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"unpair\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"unpair\",\"ok\":false,\"n\":0,\"error\":\"unsupported\"}",
        s_tx_line);

    esp_desktop_buddy_delete(buddy);
}

static void test_named_commands_require_string_name(void)
{
    buddy_handler_test_ctx_t handler_ctx = {0};
    esp_desktop_buddy_command_handlers_t handlers = {
        .ctx = &handler_ctx,
        .on_name = esp_desktop_buddy_test_name_handler,
        .on_owner = esp_desktop_buddy_test_owner_handler,
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"name\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"name\",\"ok\":false,\"n\":0,\"error\":\"invalid_request\"}",
        s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"owner\",\"name\":42}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"owner\",\"ok\":false,\"n\":0,\"error\":\"invalid_request\"}",
        s_tx_line);

    TEST_ASSERT_EQUAL_UINT32(0, handler_ctx.name_count);
    TEST_ASSERT_EQUAL_UINT32(0, handler_ctx.owner_count);

    esp_desktop_buddy_delete(buddy);
}

static void test_command_handlers_propagate_results_and_payloads(void)
{
    buddy_handler_test_ctx_t handler_ctx = {0};
    esp_desktop_buddy_command_handlers_t handlers = {
        .ctx = &handler_ctx,
        .on_name = esp_desktop_buddy_test_name_handler,
        .on_owner = esp_desktop_buddy_test_owner_handler,
        .on_unpair = esp_desktop_buddy_test_unpair_handler,
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"name\",\"name\":\"Desk\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"name\",\"ok\":true,\"n\":0}", s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"owner\",\"name\":\"Felix\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"owner\",\"ok\":false,\"n\":0,\"error\":\"rejected\"}",
        s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"unpair\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"unpair\",\"ok\":true,\"n\":0}", s_tx_line);

    TEST_ASSERT_EQUAL_UINT32(1, handler_ctx.name_count);
    TEST_ASSERT_EQUAL_UINT32(1, handler_ctx.owner_count);
    TEST_ASSERT_EQUAL_UINT32(1, handler_ctx.unpair_count);
    TEST_ASSERT_EQUAL_STRING("Desk", handler_ctx.last_name);
    TEST_ASSERT_EQUAL_STRING("Felix", handler_ctx.last_owner);

    esp_desktop_buddy_delete(buddy);
}

static void esp_desktop_buddy_delete_on_snapshot_event(void *ctx, const esp_desktop_buddy_event_t *event)
{
    esp_desktop_buddy_delete_on_event_ctx_t *delete_ctx = (esp_desktop_buddy_delete_on_event_ctx_t *)ctx;

    if (event->type != ESP_DESKTOP_BUDDY_EVENT_SNAPSHOT_UPDATED) {
        return;
    }

    delete_ctx->snapshot_count++;
    delete_ctx->delete_result = esp_desktop_buddy_delete(delete_ctx->buddy);
    xSemaphoreGive(delete_ctx->done);
}

static void test_delete_from_event_callback_does_not_deadlock(void)
{
    esp_desktop_buddy_delete_on_event_ctx_t delete_ctx = {
        .done = xSemaphoreCreateBinary(),
    };
    esp_desktop_buddy_event_listener_t event_sink = {
        .on_event = esp_desktop_buddy_delete_on_snapshot_event,
        .ctx = &delete_ctx,
    };
    esp_desktop_buddy_t *buddy;

    TEST_ASSERT_NOT_NULL(delete_ctx.done);
    buddy = esp_desktop_buddy_test_new_with_event_sink(NULL, event_sink);
    delete_ctx.buddy = buddy;

    esp_desktop_buddy_test_feed_line(
        buddy,
        "{\"total\":1,\"running\":1,\"waiting\":0,\"msg\":\"delete\",\"tokens\":1,\"tokens_today\":1}\n");
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(delete_ctx.done, pdMS_TO_TICKS(500)));
    TEST_ASSERT_EQUAL_UINT32(1, delete_ctx.snapshot_count);
    TEST_ASSERT_EQUAL(ESP_OK, delete_ctx.delete_result);
    vTaskDelay(pdMS_TO_TICKS(50));

    vSemaphoreDelete(delete_ctx.done);
}

static void test_inbound_ack_is_rejected_and_permission_is_ignored(void)
{
    static const char *frames =
        "{\"ack\":\"status\",\"ok\":true,\"n\":0}\n"
        "{\"cmd\":\"permission\",\"id\":\"p1\",\"decision\":\"once\"}\n";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_bytes(buddy, (const uint8_t *)frames, strlen(frames));
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(0, s_events.snapshot_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.prompt_response_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.turn_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_events.time_sync_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);
    esp_desktop_buddy_test_assert_no_tx(buddy);

    esp_desktop_buddy_delete(buddy);
}

static void test_line_overflow_drops_until_newline_and_recovers(void)
{
    static const char *snapshot_line =
        "{\"total\":9,\"running\":2,\"waiting\":7,\"msg\":\"recovered\","
        "\"tokens\":11,\"tokens_today\":12}\n";
    size_t oversized_len = ESP_DESKTOP_BUDDY_LINE_MAX + 16;
    char *oversized = malloc(oversized_len + 1);
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    buddy_snapshot_t snapshot = {0};
    size_t offset = 0;

    TEST_ASSERT_NOT_NULL(oversized);
    memset(oversized, 'x', oversized_len);
    oversized[oversized_len] = '\n';

    esp_desktop_buddy_test_reset_events();
    while (offset < oversized_len + 1) {
        size_t chunk_len = oversized_len + 1 - offset;
        if (chunk_len > CONFIG_ESP_DESKTOP_BUDDY_FEED_RX_CHUNK_MAX) {
            chunk_len = CONFIG_ESP_DESKTOP_BUDDY_FEED_RX_CHUNK_MAX;
        }
        esp_desktop_buddy_test_feed_bytes(buddy, (const uint8_t *)oversized + offset, chunk_len);
        offset += chunk_len;
        esp_desktop_buddy_test_wait();
    }

    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);
    TEST_ASSERT_EQUAL_UINT32(ESP_DESKTOP_BUDDY_LINE_MAX, s_events.last_error.detail);

    esp_desktop_buddy_test_feed_line(buddy, snapshot_line);
    esp_desktop_buddy_test_wait();
    TEST_ASSERT_EQUAL_UINT32(1, s_events.snapshot_count);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_capture_snapshot(buddy, &snapshot));
    TEST_ASSERT_EQUAL_UINT32(9, snapshot.total);
    TEST_ASSERT_EQUAL_STRING("recovered", snapshot.msg);

    esp_desktop_buddy_delete(buddy);
    free(oversized);
}

static void test_crlf_and_multi_frame_chunk_are_supported(void)
{
    static const char *frames =
        "{\"total\":2,\"running\":1,\"waiting\":1,\"msg\":\"chunked\","
        "\"tokens\":3,\"tokens_today\":4}\r\n"
        "{\"time\":[1713571200,19800]}\n"
        "{\"evt\":\"turn\",\"role\":\"assistant\",\"content\":{\"text\":\"chunk\"}}\r";
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_bytes(buddy, (const uint8_t *)frames, strlen(frames));
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.snapshot_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.time_sync_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.turn_count);
    TEST_ASSERT_EQUAL_STRING("chunked", s_events.last_snapshot.msg);
    TEST_ASSERT_EQUAL_STRING("{\"text\":\"chunk\"}", s_events.last_turn_content);

    esp_desktop_buddy_delete(buddy);
}

static void test_malformed_time_sync_is_rejected(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "{\"time\":[\"oops\",19800]}\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(0, s_events.time_sync_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);

    esp_desktop_buddy_delete(buddy);
}

static void test_malformed_turn_without_content_is_rejected(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "{\"evt\":\"turn\",\"role\":\"assistant\"}\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(0, s_events.turn_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);

    esp_desktop_buddy_delete(buddy);
}

static void test_non_object_json_is_rejected(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "[]\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.error_count);
    TEST_ASSERT_EQUAL(ESP_DESKTOP_BUDDY_ERROR_INPUT, s_events.last_error.kind);

    esp_desktop_buddy_delete(buddy);
}

static void test_feed_rx_bytes_rejects_oversized_chunk(void)
{
    uint8_t *bytes = calloc(1, CONFIG_ESP_DESKTOP_BUDDY_FEED_RX_CHUNK_MAX + 1);
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      esp_desktop_buddy_receive_bytes(buddy, bytes, CONFIG_ESP_DESKTOP_BUDDY_FEED_RX_CHUNK_MAX + 1));

    esp_desktop_buddy_delete(buddy);
    free(bytes);
}

static void test_nonblocking_inbox_saturation_returns_no_mem(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    esp_desktop_buddy_permission_reply_t action = {
        .type = ESP_DESKTOP_BUDDY_PERMISSION_REPLY_ONCE,
        .prompt_id = "p1",
    };

    esp_desktop_buddy_test_fill_inbox_until_full(buddy);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM,
                      esp_desktop_buddy_receive_bytes(buddy, (const uint8_t *)"x", 1));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, esp_desktop_buddy_post_permission_reply(buddy, &action));
    xTaskResumeAll();

    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_delete(buddy);
}

static void test_entry_count_reports_not_found_without_snapshot(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);
    size_t entry_count = 123;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, esp_desktop_buddy_get_entry_count(buddy, &entry_count));
    TEST_ASSERT_EQUAL_UINT32(0, entry_count);

    esp_desktop_buddy_delete(buddy);
}

static void test_time_sync_event_decodes(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(buddy, "{\"time\":[1713571200,19800]}\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.time_sync_count);
    TEST_ASSERT_TRUE_MESSAGE(s_events.last_time_sync.epoch == 1713571200LL,
                             "unexpected time sync epoch");
    TEST_ASSERT_EQUAL_INT32(19800, s_events.last_time_sync.tz_offset);

    esp_desktop_buddy_delete(buddy);
}

static void test_turn_event_content_is_available_for_callback_duration(void)
{
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(NULL);

    esp_desktop_buddy_test_reset_events();
    esp_desktop_buddy_test_feed_line(
        buddy, "{\"evt\":\"turn\",\"role\":\"assistant\",\"content\":{\"text\":\"hi\"}}\n");
    esp_desktop_buddy_test_wait();

    TEST_ASSERT_EQUAL_UINT32(1, s_events.turn_count);
    TEST_ASSERT_EQUAL_STRING("assistant", s_events.last_turn_role);
    TEST_ASSERT_EQUAL_STRING("{\"text\":\"hi\"}", s_events.last_turn_content);

    esp_desktop_buddy_delete(buddy);
}

static void test_folder_push_commands_route_through_public_api(void)
{
    buddy_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = NULL;
    esp_desktop_buddy_command_handlers_t handlers = {0};
    esp_desktop_buddy_t *buddy;

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_new_push(&mock, &push));
    handlers.command_extension = esp_desktop_buddy_folder_push_command_extension(push);
    buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"char_begin\",\"name\":\"demo\",\"total\":3}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"char_begin\",\"ok\":true,\"n\":0}", s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"file\",\"path\":\"demo.bin\",\"size\":3}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"file\",\"ok\":true,\"n\":0}", s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"chunk\",\"d\":\"YWJj\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"chunk\",\"ok\":true,\"n\":3}", s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"file_end\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"file_end\",\"ok\":true,\"n\":3}", s_tx_line);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"char_end\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"char_end\",\"ok\":true,\"n\":0}", s_tx_line);

    TEST_ASSERT_EQUAL_UINT32(1, mock.begin_transfer_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.begin_file_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_chunk_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.end_file_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.end_transfer_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);
    TEST_ASSERT_EQUAL_STRING("demo", mock.last_transfer_name);
    TEST_ASSERT_EQUAL_UINT32(3, mock.last_total_bytes);
    TEST_ASSERT_EQUAL_STRING("demo.bin", mock.last_path);
    TEST_ASSERT_EQUAL_UINT32(3, mock.last_file_size);
    TEST_ASSERT_EQUAL_UINT32(3, mock.last_bytes_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"abc", mock.last_bytes, 3);

    esp_desktop_buddy_delete(buddy);
    esp_desktop_buddy_folder_push_delete(push);
}

static void test_folder_push_invalid_request_is_nacked(void)
{
    buddy_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = NULL;
    esp_desktop_buddy_command_handlers_t handlers = {0};
    esp_desktop_buddy_t *buddy;

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_new_push(&mock, &push));
    handlers.command_extension = esp_desktop_buddy_folder_push_command_extension(push);
    buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"char_begin\",\"name\":\"demo\"}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));

    TEST_ASSERT_EQUAL_STRING(
        "{\"ack\":\"char_begin\",\"ok\":false,\"n\":0,\"error\":\"invalid_request\"}",
        s_tx_line);
    TEST_ASSERT_EQUAL_UINT32(0, mock.begin_transfer_count);

    esp_desktop_buddy_delete(buddy);
    esp_desktop_buddy_folder_push_delete(push);
}

static void test_folder_push_numeric_sizes_are_clamped_and_truncated(void)
{
    buddy_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = NULL;
    esp_desktop_buddy_command_handlers_t handlers = {0};
    esp_desktop_buddy_t *buddy;

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_test_new_push(&mock, &push));
    handlers.command_extension = esp_desktop_buddy_folder_push_command_extension(push);
    buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"char_begin\",\"name\":\"demo\",\"total\":-1}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"char_begin\",\"ok\":true,\"n\":0}", s_tx_line);
    TEST_ASSERT_EQUAL_UINT32(1, mock.begin_transfer_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.last_total_bytes);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"file\",\"path\":\"demo.bin\",\"size\":0.75}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_read_tx_line(buddy, s_tx_line, sizeof(s_tx_line));
    TEST_ASSERT_EQUAL_STRING("{\"ack\":\"file\",\"ok\":true,\"n\":0}", s_tx_line);
    TEST_ASSERT_EQUAL_UINT32(1, mock.begin_file_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.last_file_size);

    esp_desktop_buddy_delete(buddy);
    esp_desktop_buddy_folder_push_delete(push);
}

static void test_folder_push_refusal_consumes_char_begin_without_ack(void)
{
    esp_desktop_buddy_command_handlers_t handlers = {
        .command_extension = esp_desktop_buddy_folder_push_command_extension(NULL),
    };
    esp_desktop_buddy_t *buddy = esp_desktop_buddy_test_new(&handlers);

    esp_desktop_buddy_test_feed_line(buddy, "{\"cmd\":\"char_begin\",\"name\":\"demo\",\"total\":3}\n");
    esp_desktop_buddy_test_wait();
    esp_desktop_buddy_test_assert_no_tx(buddy);

    esp_desktop_buddy_delete(buddy);
}

void app_main(void)
{
    s_events.mutex = xSemaphoreCreateMutex();
    if (s_events.mutex == NULL) {
        abort();
    }

    UNITY_BEGIN();
    RUN_TEST(test_tx_dequeue_with_small_buffer_preserves_frame);
    RUN_TEST(test_snapshot_entries_skip_invalid_items_and_prompt_is_retained);
    RUN_TEST(test_snapshot_replacement_clears_optional_fields);
    RUN_TEST(test_overlong_prompt_id_rejects_snapshot);
    RUN_TEST(test_malformed_prompt_degrades_to_absent);
    RUN_TEST(test_liveness_timeout_keeps_last_snapshot);
    RUN_TEST(test_malformed_required_snapshot_is_rejected);
    RUN_TEST(test_snapshot_token_values_are_clamped_and_truncated);
    RUN_TEST(test_delete_refuses_attached_transport);
    RUN_TEST(test_snapshot_counter_values_are_clamped_and_truncated);
    RUN_TEST(test_permission_reply_is_encoded_and_emitted);
    RUN_TEST(test_same_prompt_snapshot_allows_permission_retry);
    RUN_TEST(test_stale_prompt_action_is_rejected_without_tx);
    RUN_TEST(test_expired_prompt_action_is_rejected_without_tx);
    RUN_TEST(test_post_action_rejects_invalid_type);
    RUN_TEST(test_invalid_status_data_falls_back_to_negative_ack);
    RUN_TEST(test_status_handler_errors_are_mapped_to_stable_tokens);
    RUN_TEST(test_status_too_large_falls_back_to_negative_ack);
    RUN_TEST(test_unknown_command_is_nacked);
    RUN_TEST(test_command_extension_can_consume_without_ack);
    RUN_TEST(test_command_request_get_string_rejects_null_output);
    RUN_TEST(test_unsupported_commands_are_nacked);
    RUN_TEST(test_named_commands_require_string_name);
    RUN_TEST(test_command_handlers_propagate_results_and_payloads);
    RUN_TEST(test_delete_from_event_callback_does_not_deadlock);
    RUN_TEST(test_inbound_ack_is_rejected_and_permission_is_ignored);
    RUN_TEST(test_line_overflow_drops_until_newline_and_recovers);
    RUN_TEST(test_crlf_and_multi_frame_chunk_are_supported);
    RUN_TEST(test_time_sync_event_decodes);
    RUN_TEST(test_time_sync_values_are_clamped);
    RUN_TEST(test_time_sync_fractional_values_are_truncated);
    RUN_TEST(test_malformed_time_sync_is_rejected);
    RUN_TEST(test_turn_event_content_is_available_for_callback_duration);
    RUN_TEST(test_malformed_turn_without_content_is_rejected);
    RUN_TEST(test_non_object_json_is_rejected);
    RUN_TEST(test_feed_rx_bytes_rejects_oversized_chunk);
    RUN_TEST(test_nonblocking_inbox_saturation_returns_no_mem);
    RUN_TEST(test_entry_count_reports_not_found_without_snapshot);
    RUN_TEST(test_folder_push_commands_route_through_public_api);
    RUN_TEST(test_folder_push_invalid_request_is_nacked);
    RUN_TEST(test_folder_push_numeric_sizes_are_clamped_and_truncated);
    RUN_TEST(test_folder_push_refusal_consumes_char_begin_without_ack);
    UNITY_END();

    vSemaphoreDelete(s_events.mutex);
}
