/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/folder_push.h"
#include "esp_desktop_buddy/transport_ble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "example_app_helpers.h"
#include "example_charpack.h"

#define BOX_DEMO_NAME_MAX 32
#define BOX_DEMO_BLE_NAME_MAX 32
#define BOX_DEMO_OWNER_MAX 32
#define BOX_DEMO_STATUS_MAX 64

typedef struct {
    SemaphoreHandle_t mutex;
    esp_desktop_buddy_t *buddy;
    esp_desktop_buddy_transport_ble_t *transport;
    esp_desktop_buddy_folder_push_t *folder_push;
    example_charpack_t *charpack;
    example_buddy_state_cache_t state_cache;
    char display_name[BOX_DEMO_NAME_MAX];
    char advertising_name[BOX_DEMO_BLE_NAME_MAX];
    char owner_name[BOX_DEMO_OWNER_MAX];
    char pack_status[BOX_DEMO_STATUS_MAX];
    bool have_active_pack;
    example_charpack_info_t active_pack;
    esp_desktop_buddy_transport_ble_state_t transport_state;
    uint32_t approval_count;
    uint32_t denial_count;
    uint32_t nap_seconds;
    example_progress_state_t progress;
    int32_t tz_offset_seconds;
} box_demo_app_t;

void box_demo_app_init(box_demo_app_t *app);

esp_desktop_buddy_status_reply_t box_demo_status_handler(void *ctx, esp_desktop_buddy_t *buddy);
esp_desktop_buddy_command_result_t box_demo_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name);
esp_desktop_buddy_command_result_t box_demo_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name);
esp_desktop_buddy_command_result_t box_demo_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy);
void box_demo_buddy_event(void *ctx, const esp_desktop_buddy_event_t *event);
void box_demo_transport_event(void *ctx, const esp_desktop_buddy_transport_ble_event_t *event);
void box_demo_charpack_event(void *ctx, const example_charpack_event_t *event);

void box_demo_print_state(box_demo_app_t *app, FILE *out);

esp_err_t box_demo_ui_init(box_demo_app_t *app);
void box_demo_ui_start(box_demo_app_t *app);
void box_demo_charpack_console_start(box_demo_app_t *app);
