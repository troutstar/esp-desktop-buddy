#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "example_app_helpers.h"
#include "example_status.h"

#include "app_commands.h"

#define TAG "cyd_buddy"
#define NVS_NS  "buddy"
#define STATUS_BUF (ESP_DESKTOP_BUDDY_LINE_MAX + 1)

void cyd_app_init(cyd_app_t *app)
{
    memset(app, 0, sizeof(*app));
    example_safe_copy(app->display_name, sizeof(app->display_name), "CYD Buddy");
}

esp_desktop_buddy_status_reply_t cyd_status_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    cyd_app_t *app = (cyd_app_t *)ctx;
    static char status_json[STATUS_BUF];
    esp_desktop_buddy_transport_ble_state_t transport_state = {0};
    example_buddy_state_cache_t state_cache = {0};
    char display_name[CYD_APP_NAME_MAX];
    char owner_name[CYD_APP_OWNER_MAX];
    uint32_t approvals;
    uint32_t denials;
    uint32_t level;
    uint32_t velocity;
    int32_t tz_offset_seconds;
    example_progress_state_t progress = {0};
    example_status_doc_t doc = {0};
    cJSON *bat = NULL;
    cJSON *buddy_state = NULL;
    cJSON *entries = NULL;
    cJSON *prompt = NULL;
    cJSON *turn = NULL;
    (void)buddy;

    if (app->transport != NULL) {
        esp_desktop_buddy_transport_ble_get_state(app->transport, &transport_state);
    }

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    example_safe_copy(display_name, sizeof(display_name), app->display_name);
    example_safe_copy(owner_name, sizeof(owner_name), app->owner_name);
    state_cache = app->state_cache;
    approvals = app->approval_count;
    denials = app->denial_count;
    tz_offset_seconds = app->tz_offset_seconds;
    progress = app->progress;
    xSemaphoreGive(app->mutex);

    level = example_progress_level(state_cache.tokens);
    velocity = example_progress_velocity(&progress);

    if (example_status_begin(&doc, display_name, owner_name,
                              transport_state.encrypted, approvals, denials) != ESP_OK) {
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_begin_failed");
    }

    bat = cJSON_AddObjectToObject(doc.root, "bat");
    buddy_state = cJSON_AddObjectToObject(doc.root, "buddy");
    if (bat == NULL || buddy_state == NULL) {
        cJSON_Delete(doc.root);
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_section_alloc_failed");
    }
    entries = cJSON_AddArrayToObject(buddy_state, "entries");
    prompt = cJSON_AddObjectToObject(buddy_state, "prompt");
    turn = cJSON_AddObjectToObject(buddy_state, "turn");
    if (entries == NULL || prompt == NULL || turn == NULL) {
        cJSON_Delete(doc.root);
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_section_alloc_failed");
    }

    cJSON_AddNumberToObject(bat, "pct", 0);
    cJSON_AddNumberToObject(bat, "mV", 0);
    cJSON_AddNumberToObject(bat, "mA", 0);
    cJSON_AddBoolToObject(bat, "usb", false);
    cJSON_AddNumberToObject(doc.stats, "vel", (double)velocity);
    cJSON_AddNumberToObject(doc.stats, "nap", 0);
    cJSON_AddNumberToObject(doc.stats, "lvl", (double)level);
    cJSON_AddBoolToObject(buddy_state, "haveState", state_cache.has_state);
    cJSON_AddBoolToObject(buddy_state, "live", app->live);
    cJSON_AddNumberToObject(buddy_state, "tz", (double)tz_offset_seconds);
    cJSON_AddNumberToObject(buddy_state, "total", (double)state_cache.total);
    cJSON_AddNumberToObject(buddy_state, "running", (double)state_cache.running);
    cJSON_AddNumberToObject(buddy_state, "waiting", (double)state_cache.waiting);
    cJSON_AddNumberToObject(buddy_state, "tokens", (double)state_cache.tokens);
    cJSON_AddNumberToObject(buddy_state, "tokensToday", (double)state_cache.tokens_today);
    cJSON_AddStringToObject(buddy_state, "msg", state_cache.msg);
    for (size_t i = 0; i < state_cache.entry_count; i++) {
        cJSON_AddItemToArray(entries, cJSON_CreateString(state_cache.entries[i]));
    }
    cJSON_AddBoolToObject(prompt, "present", state_cache.prompt.present);
    cJSON_AddStringToObject(prompt, "id", state_cache.prompt.id);
    cJSON_AddStringToObject(prompt, "tool", state_cache.prompt.tool);
    cJSON_AddStringToObject(prompt, "hint", state_cache.prompt.hint);
    cJSON_AddStringToObject(turn, "role", "");
    cJSON_AddNumberToObject(turn, "len", 0);

    return example_status_from_json(doc.root, status_json, sizeof(status_json));
}

esp_desktop_buddy_command_result_t cyd_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    cyd_app_t *app = (cyd_app_t *)ctx;
    (void)buddy;
    return example_update_persisted_string(app->mutex, app->display_name,
                                           sizeof(app->display_name), name, NVS_NS, "display_name");
}

esp_desktop_buddy_command_result_t cyd_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    cyd_app_t *app = (cyd_app_t *)ctx;
    (void)buddy;
    return example_update_string_field(app->mutex, app->owner_name, sizeof(app->owner_name), name);
}

esp_desktop_buddy_command_result_t cyd_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    cyd_app_t *app = (cyd_app_t *)ctx;
    (void)buddy;
    return example_clear_bonds(app->transport);
}

void cyd_buddy_event(void *ctx, const esp_desktop_buddy_event_t *event)
{
    cyd_app_t *app = (cyd_app_t *)ctx;

    if (app == NULL || event == NULL) {
        return;
    }

    switch (event->type) {
    case ESP_DESKTOP_BUDDY_EVENT_SNAPSHOT_UPDATED: {
        example_buddy_state_cache_t state_cache = {0};
        example_buddy_state_cache_t previous_state = {0};

        if (example_buddy_state_cache_refresh(app->buddy, &state_cache) != ESP_OK) {
            break;
        }

        xSemaphoreTake(app->mutex, portMAX_DELAY);
        previous_state = app->state_cache;
        example_progress_note_snapshot(&app->progress, &previous_state, &state_cache);
        app->state_cache = state_cache;
        app->have_state = state_cache.has_state;
        xSemaphoreGive(app->mutex);

        ESP_LOGI(TAG, "state running=%lu waiting=%lu msg=%s",
                 (unsigned long)state_cache.running,
                 (unsigned long)state_cache.waiting,
                 state_cache.msg);
        break;
    }
    case ESP_DESKTOP_BUDDY_EVENT_PERMISSION_SENT:
        xSemaphoreTake(app->mutex, portMAX_DELAY);
        example_progress_note_permission_sent(&app->progress, event->data.permission_sent.decision);
        xSemaphoreGive(app->mutex);
        example_note_prompt_response(app->mutex, &app->approval_count, &app->denial_count,
                                     event->data.permission_sent.decision);
        break;
    case ESP_DESKTOP_BUDDY_EVENT_TIME_SYNC:
        if (example_apply_time_sync(app->mutex, &app->tz_offset_seconds,
                                    &event->data.time_sync) != ESP_OK) {
            ESP_LOGW(TAG, "settimeofday failed");
        }
        break;
    case ESP_DESKTOP_BUDDY_EVENT_LIVENESS_CHANGED:
        xSemaphoreTake(app->mutex, portMAX_DELAY);
        app->live = event->data.live;
        if (!event->data.live) {
            example_progress_clear_prompt_timing(&app->progress);
        }
        xSemaphoreGive(app->mutex);
        ESP_LOGI(TAG, "buddy live=%d", event->data.live);
        break;
    case ESP_DESKTOP_BUDDY_EVENT_ERROR:
        ESP_LOGW(TAG, "buddy error kind=%d detail=%lu",
                 event->data.error.kind, (unsigned long)event->data.error.detail);
        break;
    default:
        break;
    }
}

void cyd_transport_event(void *ctx, const esp_desktop_buddy_transport_ble_event_t *event)
{
    cyd_app_t *app = (cyd_app_t *)ctx;

    if (app == NULL || event == NULL) {
        return;
    }

    example_update_transport_state(app->mutex, &app->transport_state,
                                   app->owner_name, sizeof(app->owner_name), event);

    if ((event->changed_fields & ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_PASSKEY) != 0 &&
        event->state.has_passkey) {
        ESP_LOGI(TAG, "pairing passkey %06lu", (unsigned long)event->state.passkey);
    }
}
