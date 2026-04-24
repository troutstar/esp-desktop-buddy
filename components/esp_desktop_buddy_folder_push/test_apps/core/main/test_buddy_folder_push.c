/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "unity.h"

#include "esp_desktop_buddy/folder_push.h"

#define TEST_TOKEN_INVALID_REQUEST "invalid_request"
#define TEST_TOKEN_BAD_SEQUENCE "bad_sequence"
#define TEST_TOKEN_INVALID_PATH "invalid_path"
#define TEST_TOKEN_INVALID_BASE64 "invalid_base64"
#define TEST_TOKEN_TRANSFER_FAILED "transfer_failed"
#define TEST_TOKEN_SIZE_MISMATCH "size_mismatch"
#define TEST_TOKEN_TRANSFER_TOO_LARGE "transfer_too_large"

typedef struct {
    size_t begin_transfer_count;
    size_t begin_file_count;
    size_t write_chunk_count;
    size_t end_file_count;
    size_t end_transfer_count;
    size_t abort_count;
    bool fail_begin_transfer_invalid_content;
    bool fail_begin_file;
    bool fail_write_chunk;
    bool fail_end_transfer;
    char transfer_name[65];
    char file_path[65];
    uint32_t total_bytes;
    uint32_t file_size;
    uint8_t bytes[64];
    size_t bytes_len;
} folder_push_mock_t;

static esp_desktop_buddy_folder_push_sink_result_t folder_push_mock_begin_transfer(void *ctx,
                                                                  const char *name,
                                                                  uint32_t total_bytes)
{
    folder_push_mock_t *mock = (folder_push_mock_t *)ctx;

    mock->begin_transfer_count++;
    mock->total_bytes = total_bytes;
    strlcpy(mock->transfer_name, name, sizeof(mock->transfer_name));
    if (mock->fail_begin_transfer_invalid_content) {
        return esp_desktop_buddy_folder_push_result_err(ESP_ERR_INVALID_ARG,
                                                        ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_INVALID_CONTENT,
                                                        "invalid_pack_id");
    }
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t folder_push_mock_begin_file(void *ctx,
                                                              const char *path,
                                                              uint32_t size)
{
    folder_push_mock_t *mock = (folder_push_mock_t *)ctx;

    mock->begin_file_count++;
    mock->file_size = size;
    strlcpy(mock->file_path, path, sizeof(mock->file_path));
    if (mock->fail_begin_file) {
        return esp_desktop_buddy_folder_push_result_err(ESP_FAIL,
                                                        ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED,
                                                        "open_failed");
    }
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t folder_push_mock_write_chunk(void *ctx,
                                                               const uint8_t *data,
                                                               size_t len)
{
    folder_push_mock_t *mock = (folder_push_mock_t *)ctx;

    mock->write_chunk_count++;
    if (mock->fail_write_chunk) {
        return esp_desktop_buddy_folder_push_result_err(ESP_FAIL,
                                                        ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED,
                                                        "disk_full");
    }

    if (len > sizeof(mock->bytes)) {
        return esp_desktop_buddy_folder_push_result_err(ESP_FAIL,
                                                        ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED,
                                                        "too_large");
    }
    mock->bytes_len = len;
    memcpy(mock->bytes, data, len);
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t folder_push_mock_end_file(void *ctx)
{
    folder_push_mock_t *mock = (folder_push_mock_t *)ctx;

    mock->end_file_count++;
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t folder_push_mock_end_transfer(void *ctx)
{
    folder_push_mock_t *mock = (folder_push_mock_t *)ctx;

    mock->end_transfer_count++;
    if (mock->fail_end_transfer) {
        return esp_desktop_buddy_folder_push_result_err(ESP_FAIL,
                                                        ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED,
                                                        "commit_failed");
    }
    return esp_desktop_buddy_folder_push_result_ok();
}

static void folder_push_mock_abort(void *ctx)
{
    folder_push_mock_t *mock = (folder_push_mock_t *)ctx;

    mock->abort_count++;
}

static esp_desktop_buddy_folder_push_t *folder_push_test_new(folder_push_mock_t *mock)
{
    esp_desktop_buddy_folder_push_t *push = NULL;
    esp_desktop_buddy_folder_push_config_t config = {
        .sink = {
            .begin_transfer = folder_push_mock_begin_transfer,
            .begin_file = folder_push_mock_begin_file,
            .write_chunk = folder_push_mock_write_chunk,
            .end_file = folder_push_mock_end_file,
            .end_transfer = folder_push_mock_end_transfer,
            .abort_transfer = folder_push_mock_abort,
            .ctx = mock,
        },
    };

    memset(mock, 0, sizeof(*mock));
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_new(&config, &push));
    return push;
}

static void test_rejects_bad_sequence_before_transfer(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 1, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_BAD_SEQUENCE, reply.error);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YQ==", &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_BAD_SEQUENCE, reply.error);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_file(push, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_BAD_SEQUENCE, reply.error);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_transfer(push, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_BAD_SEQUENCE, reply.error);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_invalid_path_is_rejected_without_aborting_transfer(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 3, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "../evil", 3, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_INVALID_PATH, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 3, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_STRING("demo.bin", mock.file_path);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_transfer_size_cap_rejects_char_begin_before_sink(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};
    uint32_t oversized_total = (uint32_t)CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_TRANSFER_BYTES + 1;

    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_desktop_buddy_folder_push_start_transfer(push, "demo", oversized_total, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_TRANSFER_TOO_LARGE, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.begin_transfer_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_happy_path_counts_bytes_and_finishes(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 3, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(0, reply.n);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 3, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(0, reply.n);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YWJj", &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(3, reply.n);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_file(push, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(3, reply.n);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_transfer(push, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(0, reply.n);

    TEST_ASSERT_EQUAL_UINT32(1, mock.begin_transfer_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.begin_file_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.write_chunk_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.end_file_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.end_transfer_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);
    TEST_ASSERT_EQUAL_STRING("demo", mock.transfer_name);
    TEST_ASSERT_EQUAL_STRING("demo.bin", mock.file_path);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"abc", mock.bytes, 3);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_invalid_base64_does_not_abort_transfer(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "%%%bad%%%", &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_INVALID_BASE64, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YQ==", &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(1, reply.n);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_oversized_chunk_is_rejected_before_sink_write(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YWI=", &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(0, reply.n);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_SIZE_MISMATCH, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.write_chunk_count);
    TEST_ASSERT_EQUAL_UINT32(1, mock.abort_count);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_chunk_exceeding_decoded_capacity_is_rejected_before_decode(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};
    size_t encoded_len = (4 * ((CONFIG_ESP_DESKTOP_BUDDY_FOLDER_PUSH_MAX_DECODED_CHUNK + 2) / 3)) + 1;
    char *encoded = calloc(encoded_len + 1, 1);

    TEST_ASSERT_NOT_NULL(encoded);
    memset(encoded, 'A', encoded_len);
    encoded[encoded_len] = '\0';

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, encoded, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_INVALID_REQUEST, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.write_chunk_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);

    free(encoded);
    esp_desktop_buddy_folder_push_delete(push);
}

static void test_size_mismatch_aborts_transfer_and_resets_state(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 2, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 2, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YQ==", &reply));
    TEST_ASSERT_TRUE(reply.ok);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_file(push, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(1, reply.n);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_SIZE_MISMATCH, reply.error);
    TEST_ASSERT_EQUAL_UINT32(1, mock.abort_count);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "next", 0, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_STRING("next", mock.transfer_name);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_total_size_mismatch_on_char_end_aborts_transfer_and_resets_state(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 2, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YQ==", &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_file(push, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_transfer(push, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_UINT32(1, reply.n);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_SIZE_MISMATCH, reply.error);
    TEST_ASSERT_EQUAL_UINT32(1, mock.abort_count);
    TEST_ASSERT_EQUAL_UINT32(0, mock.end_transfer_count);

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "next", 0, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL_STRING("next", mock.transfer_name);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_sink_failure_is_mapped_and_aborts_transfer(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_file(push, "demo.bin", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    mock.fail_write_chunk = true;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_write_chunk_b64(push, "YQ==", &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_TRANSFER_FAILED, reply.error);
    TEST_ASSERT_EQUAL_UINT32(1, mock.abort_count);

    mock.fail_write_chunk = false;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "next", 1, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_char_end_failure_resets_transfer_without_abort(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 0, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    mock.fail_end_transfer = true;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_finish_transfer(push, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_TRANSFER_FAILED, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);

    mock.fail_end_transfer = false;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "next", 0, &reply));
    TEST_ASSERT_TRUE(reply.ok);

    esp_desktop_buddy_folder_push_delete(push);
}

static void test_sink_invalid_content_is_stable_locally_and_hidden_on_wire(void)
{
    folder_push_mock_t mock = {0};
    esp_desktop_buddy_folder_push_t *push = folder_push_test_new(&mock);
    esp_desktop_buddy_command_ack_t reply = {0};

    TEST_ASSERT_EQUAL_STRING("ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_INVALID_CONTENT",
                             esp_desktop_buddy_folder_push_sink_reason_to_name(
                                 ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_INVALID_CONTENT));
    TEST_ASSERT_EQUAL_STRING("ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED",
                             esp_desktop_buddy_folder_push_sink_reason_to_name(
                                 ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED));

    mock.fail_begin_transfer_invalid_content = true;
    TEST_ASSERT_EQUAL(ESP_OK, esp_desktop_buddy_folder_push_start_transfer(push, "demo", 0, &reply));
    TEST_ASSERT_FALSE(reply.ok);
    TEST_ASSERT_EQUAL_STRING(TEST_TOKEN_TRANSFER_FAILED, reply.error);
    TEST_ASSERT_EQUAL_UINT32(0, mock.abort_count);

    esp_desktop_buddy_folder_push_delete(push);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rejects_bad_sequence_before_transfer);
    RUN_TEST(test_invalid_path_is_rejected_without_aborting_transfer);
    RUN_TEST(test_transfer_size_cap_rejects_char_begin_before_sink);
    RUN_TEST(test_happy_path_counts_bytes_and_finishes);
    RUN_TEST(test_invalid_base64_does_not_abort_transfer);
    RUN_TEST(test_oversized_chunk_is_rejected_before_sink_write);
    RUN_TEST(test_chunk_exceeding_decoded_capacity_is_rejected_before_decode);
    RUN_TEST(test_size_mismatch_aborts_transfer_and_resets_state);
    RUN_TEST(test_total_size_mismatch_on_char_end_aborts_transfer_and_resets_state);
    RUN_TEST(test_sink_failure_is_mapped_and_aborts_transfer);
    RUN_TEST(test_char_end_failure_resets_transfer_without_abort);
    RUN_TEST(test_sink_invalid_content_is_stable_locally_and_hidden_on_wire);
    UNITY_END();
}
