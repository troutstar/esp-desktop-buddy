#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "esp_desktop_buddy/transport_ble.h"
#include "example_app_helpers.h"

#define CYD_APP_NAME_MAX      32
#define CYD_APP_OWNER_MAX     32
#define CYD_APP_BLE_NAME_MAX  32

struct cyd_app_s {
    SemaphoreHandle_t mutex;
    esp_desktop_buddy_t *buddy;
    esp_desktop_buddy_transport_ble_t *transport;
    example_buddy_state_cache_t state_cache;
    esp_desktop_buddy_transport_ble_state_t transport_state;
    example_progress_state_t progress;
    bool have_state;
    bool live;
    char display_name[CYD_APP_NAME_MAX];
    char owner_name[CYD_APP_OWNER_MAX];
    char advertising_name[CYD_APP_BLE_NAME_MAX];
    uint32_t approval_count;
    uint32_t denial_count;
    int32_t tz_offset_seconds;
};

typedef struct cyd_app_s cyd_app_t;

void cyd_app_init(cyd_app_t *app);

esp_desktop_buddy_status_reply_t cyd_status_handler(void *ctx, esp_desktop_buddy_t *buddy);
esp_desktop_buddy_command_result_t cyd_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name);
esp_desktop_buddy_command_result_t cyd_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name);
esp_desktop_buddy_command_result_t cyd_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy);
void cyd_buddy_event(void *ctx, const esp_desktop_buddy_event_t *event);
void cyd_transport_event(void *ctx, const esp_desktop_buddy_transport_ble_event_t *event);
