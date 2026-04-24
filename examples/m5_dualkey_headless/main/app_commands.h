/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdio.h>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/transport_ble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "example_app_helpers.h"

#include "board_hw.h"

#define M5_DUALKEY_NAME_MAX 32
#define M5_DUALKEY_OWNER_MAX 32

typedef struct {
    SemaphoreHandle_t mutex;
    esp_desktop_buddy_t *buddy;
    esp_desktop_buddy_transport_ble_t *transport;
    example_buddy_state_cache_t state_cache;
    bool have_state;
    bool live;
    char display_name[M5_DUALKEY_NAME_MAX];
    char owner_name[M5_DUALKEY_OWNER_MAX];
    uint32_t approval_count;
    uint32_t denial_count;
    uint32_t turn_count;
    int32_t tz_offset_seconds;
    uint32_t last_turn_len;
    char last_turn_role[ESP_DESKTOP_BUDDY_TURN_ROLE_MAX + 1];
    example_progress_state_t progress;
    esp_desktop_buddy_transport_ble_state_t transport_state;
    bool left_key_pressed;
    bool right_key_pressed;
    m5_dualkey_board_t board;
} m5_dualkey_app_t;

void m5_dualkey_app_init(m5_dualkey_app_t *app);

esp_desktop_buddy_status_reply_t m5_dualkey_status_handler(void *ctx, esp_desktop_buddy_t *buddy);
esp_desktop_buddy_command_result_t m5_dualkey_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name);
esp_desktop_buddy_command_result_t m5_dualkey_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name);
esp_desktop_buddy_command_result_t m5_dualkey_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy);
void m5_dualkey_buddy_event(void *ctx, const esp_desktop_buddy_event_t *event);
void m5_dualkey_transport_event(void *ctx, const esp_desktop_buddy_transport_ble_event_t *event);

void m5_dualkey_print_state(m5_dualkey_app_t *app, FILE *out);
