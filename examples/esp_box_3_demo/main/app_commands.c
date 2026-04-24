/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "example_app_helpers.h"
#include "example_status.h"

#include "app_shared.h"

#define BOX_DEMO_NVS_NAMESPACE "buddy"
#define BOX_DEMO_STATUS_BUF (ESP_DESKTOP_BUDDY_LINE_MAX + 1)

static const char *TAG = "esp_box_3_demo";

static const char *box_demo_pack_mode_name(example_charpack_mode_t mode)
{
    switch (mode) {
    case EXAMPLE_CHARPACK_MODE_GIF:
        return "gif";
    case EXAMPLE_CHARPACK_MODE_TEXT:
        return "text";
    default:
        return "unknown";
    }
}

void box_demo_app_init(box_demo_app_t *app)
{
    if (app == NULL) {
        return;
    }

    memset(app, 0, sizeof(*app));
    example_safe_copy(app->display_name, sizeof(app->display_name), "Box Buddy");
    app->advertising_name[0] = '\0';
    example_safe_copy(app->pack_status, sizeof(app->pack_status), "No active pack");
}

esp_desktop_buddy_status_reply_t box_demo_status_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;
    static char status_json[BOX_DEMO_STATUS_BUF];
    esp_desktop_buddy_transport_ble_state_t transport_state = {0};
    char display_name[BOX_DEMO_NAME_MAX];
    char owner_name[BOX_DEMO_OWNER_MAX];
    char pack_status[BOX_DEMO_STATUS_MAX];
    uint32_t approvals;
    uint32_t denials;
    uint32_t nap_seconds;
    uint32_t level;
    uint32_t velocity;
    uint64_t fs_total = 0;
    uint64_t fs_free = 0;
    uint64_t tokens;
    bool have_active_pack;
    example_charpack_info_t active_pack = {0};
    example_progress_state_t progress = {0};
    example_status_doc_t doc = {0};
    cJSON *bat = NULL;
    cJSON *pack = NULL;
    (void)buddy;

    if (app->transport != NULL) {
        esp_desktop_buddy_transport_ble_get_state(app->transport, &transport_state);
    }

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    example_safe_copy(display_name, sizeof(display_name), app->display_name);
    example_safe_copy(owner_name, sizeof(owner_name), app->owner_name);
    example_safe_copy(pack_status, sizeof(pack_status), app->pack_status);
    approvals = app->approval_count;
    denials = app->denial_count;
    nap_seconds = app->nap_seconds;
    tokens = app->state_cache.tokens;
    have_active_pack = app->have_active_pack;
    active_pack = app->active_pack;
    progress = app->progress;
    xSemaphoreGive(app->mutex);

    level = example_progress_level(tokens);
    velocity = example_progress_velocity(&progress);

    (void)esp_vfs_fat_info(CONFIG_EXAMPLE_CHARPACK_MOUNT_POINT, &fs_total, &fs_free);

    if (example_status_begin(&doc,
                               display_name,
                               owner_name,
                               transport_state.encrypted,
                               approvals,
                               denials) != ESP_OK) {
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_begin_failed");
    }
    bat = cJSON_AddObjectToObject(doc.root, "bat");
    pack = cJSON_AddObjectToObject(doc.root, "pack");
    if (bat == NULL || pack == NULL) {
        cJSON_Delete(doc.root);
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_section_alloc_failed");
    }
    cJSON_AddNumberToObject(bat, "pct", 0);
    cJSON_AddNumberToObject(bat, "mV", 0);
    cJSON_AddNumberToObject(bat, "mA", 0);
    cJSON_AddBoolToObject(bat, "usb", false);
    cJSON_AddNumberToObject(doc.sys, "fsFree", (double)fs_free);
    cJSON_AddNumberToObject(doc.sys, "fsTotal", (double)fs_total);
    cJSON_AddNumberToObject(doc.stats, "vel", (double)velocity);
    cJSON_AddNumberToObject(doc.stats, "nap", (double)nap_seconds);
    cJSON_AddNumberToObject(doc.stats, "lvl", (double)level);
    cJSON_AddBoolToObject(pack, "hasActive", have_active_pack);
    cJSON_AddStringToObject(pack, "active", have_active_pack ? active_pack.pack_id : "");
    cJSON_AddStringToObject(pack,
                            "mode",
                            have_active_pack ? box_demo_pack_mode_name(active_pack.mode) : "");
    cJSON_AddStringToObject(pack, "status", pack_status);

    return example_status_from_json(doc.root, status_json, sizeof(status_json));
}

esp_desktop_buddy_command_result_t box_demo_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;

    (void)buddy;
    return example_update_persisted_string(app->mutex,
                                             app->display_name,
                                             sizeof(app->display_name),
                                             name,
                                             BOX_DEMO_NVS_NAMESPACE,
                                             "display_name");
}

esp_desktop_buddy_command_result_t box_demo_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;

    (void)buddy;
    return example_update_string_field(app->mutex,
                                       app->owner_name,
                                       sizeof(app->owner_name),
                                       name);
}

esp_desktop_buddy_command_result_t box_demo_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;

    (void)buddy;
    return example_clear_bonds(app->transport);
}

void box_demo_buddy_event(void *ctx, const esp_desktop_buddy_event_t *event)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;

    if (app == NULL || event == NULL) {
        return;
    }

    switch (event->type) {
    case ESP_DESKTOP_BUDDY_EVENT_SNAPSHOT_UPDATED: {
        example_buddy_state_cache_t previous_state = {0};
        example_buddy_state_cache_t state_cache = {0};

        if (example_buddy_state_cache_refresh(app->buddy, &state_cache) != ESP_OK) {
            break;
        }
        xSemaphoreTake(app->mutex, portMAX_DELAY);
        previous_state = app->state_cache;
        example_progress_note_snapshot(&app->progress, &previous_state, &state_cache);
        app->state_cache = state_cache;
        xSemaphoreGive(app->mutex);
        break;
    }
    case ESP_DESKTOP_BUDDY_EVENT_LIVENESS_CHANGED:
        if (!event->data.live) {
            xSemaphoreTake(app->mutex, portMAX_DELAY);
            example_buddy_state_cache_reset(&app->state_cache);
            example_progress_clear_prompt_timing(&app->progress);
            xSemaphoreGive(app->mutex);
        }
        break;
    case ESP_DESKTOP_BUDDY_EVENT_PERMISSION_SENT:
        xSemaphoreTake(app->mutex, portMAX_DELAY);
        example_progress_note_permission_sent(&app->progress, event->data.permission_sent.decision);
        xSemaphoreGive(app->mutex);
        example_note_prompt_response(app->mutex,
                                       &app->approval_count,
                                       &app->denial_count,
                                       event->data.permission_sent.decision);
        break;
    case ESP_DESKTOP_BUDDY_EVENT_TIME_SYNC:
        (void)example_apply_time_sync(app->mutex,
                                        &app->tz_offset_seconds,
                                        &event->data.time_sync);
        break;
    case ESP_DESKTOP_BUDDY_EVENT_ERROR:
        ESP_LOGW(TAG,
                 "buddy error kind=%d detail=%lu",
                 event->data.error.kind,
                 (unsigned long)event->data.error.detail);
        break;
    default:
        break;
    }
}

void box_demo_transport_event(void *ctx, const esp_desktop_buddy_transport_ble_event_t *event)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;

    if (app == NULL || event == NULL) {
        return;
    }

    example_update_transport_state(app->mutex,
                                   &app->transport_state,
                                   NULL,
                                   0,
                                   event);
}

void box_demo_charpack_event(void *ctx, const example_charpack_event_t *event)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;
    example_charpack_info_t active_info = {0};
    bool refresh_active = false;

    if (app == NULL || event == NULL) {
        return;
    }

    if (event->type == EXAMPLE_CHARPACK_EVENT_INSTALL_SUCCEEDED &&
        app->charpack != NULL &&
        example_charpack_get_active(app->charpack, &active_info) == ESP_OK &&
        strcmp(active_info.pack_id, event->info.pack_id) == 0) {
        refresh_active = true;
    }

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    switch (event->type) {
    case EXAMPLE_CHARPACK_EVENT_INSTALL_SUCCEEDED:
        if (refresh_active) {
            app->active_pack = active_info;
            app->have_active_pack = true;
        }
        snprintf(app->pack_status, sizeof(app->pack_status), "Installed %s", event->info.pack_id);
        break;
    case EXAMPLE_CHARPACK_EVENT_INSTALL_FAILED:
        snprintf(app->pack_status, sizeof(app->pack_status), "Install failed %s", event->info.pack_id);
        break;
    case EXAMPLE_CHARPACK_EVENT_TRANSFER_STARTED:
        snprintf(app->pack_status, sizeof(app->pack_status), "Receiving %s", event->info.pack_id);
        break;
    case EXAMPLE_CHARPACK_EVENT_ACTIVE_CHANGED:
        app->active_pack = event->info;
        app->have_active_pack = true;
        snprintf(app->pack_status, sizeof(app->pack_status), "Active %s", event->info.pack_id);
        break;
    case EXAMPLE_CHARPACK_EVENT_ACTIVE_CLEARED:
        app->have_active_pack = false;
        snprintf(app->pack_status, sizeof(app->pack_status), "No active pack");
        break;
    default:
        break;
    }
    xSemaphoreGive(app->mutex);
}

void box_demo_print_state(box_demo_app_t *app, FILE *out)
{
    example_buddy_state_cache_t state_cache = {0};
    esp_desktop_buddy_transport_ble_state_t transport = {0};
    example_charpack_info_t active = {0};
    bool have_active;
    char display_name[BOX_DEMO_NAME_MAX];
    char advertising_name[BOX_DEMO_BLE_NAME_MAX];
    char owner_name[BOX_DEMO_OWNER_MAX];
    char pack_status[BOX_DEMO_STATUS_MAX];

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    state_cache = app->state_cache;
    transport = app->transport_state;
    active = app->active_pack;
    have_active = app->have_active_pack;
    example_safe_copy(display_name, sizeof(display_name), app->display_name);
    example_safe_copy(advertising_name, sizeof(advertising_name), app->advertising_name);
    example_safe_copy(owner_name, sizeof(owner_name), app->owner_name);
    example_safe_copy(pack_status, sizeof(pack_status), app->pack_status);
    xSemaphoreGive(app->mutex);

    fprintf(out,
            "display=%s ble_name=%s owner=%s tx_ready=%d encrypted=%d active_pack=%s status=%s\n",
            display_name,
            advertising_name[0] != '\0' ? advertising_name : "<unknown>",
            owner_name,
            transport.tx_ready,
            transport.encrypted,
            have_active ? active.pack_id : "<none>",
            pack_status);
    if (!state_cache.has_state) {
        fprintf(out, "state=<none>\n");
        fflush(out);
        return;
    }
    fprintf(out,
            "state total=%lu running=%lu waiting=%lu msg=%s\n",
            (unsigned long)state_cache.total,
            (unsigned long)state_cache.running,
            (unsigned long)state_cache.waiting,
            state_cache.msg);
    if (state_cache.prompt.present) {
        fprintf(out, "prompt=%s tool=%s hint=%s\n",
                state_cache.prompt.id,
                state_cache.prompt.tool,
                state_cache.prompt.hint);
    }
    fflush(out);
}
