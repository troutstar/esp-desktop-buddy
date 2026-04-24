/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "example_app_helpers.h"
#include "example_status.h"

#include "app_commands.h"

#define M5_DUALKEY_NVS_NAMESPACE "buddy"
#define M5_DUALKEY_STATUS_BUF (ESP_DESKTOP_BUDDY_LINE_MAX + 1)

static const char *TAG = "m5_dualkey";

static const char *m5_dualkey_attention_label(const example_buddy_state_cache_t *state_cache,
                                              const esp_desktop_buddy_transport_ble_state_t *transport_state)
{
    if (transport_state->has_passkey) {
        return "pairing";
    }
    if (state_cache->prompt.present) {
        return "prompt";
    }
    return "idle";
}

void m5_dualkey_app_init(m5_dualkey_app_t *app)
{
    if (app == NULL) {
        return;
    }

    memset(app, 0, sizeof(*app));
    example_safe_copy(app->display_name, sizeof(app->display_name), "DualKey Buddy");
}

esp_desktop_buddy_status_reply_t m5_dualkey_status_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;
    static char status_json[M5_DUALKEY_STATUS_BUF];
    esp_desktop_buddy_transport_ble_state_t transport_state = {0};
    example_buddy_state_cache_t state_cache = {0};
    char display_name[M5_DUALKEY_NAME_MAX];
    char owner_name[M5_DUALKEY_OWNER_MAX];
    char last_turn_role[ESP_DESKTOP_BUDDY_TURN_ROLE_MAX + 1];
    uint32_t approvals;
    uint32_t denials;
    uint32_t last_turn_len;
    uint32_t level;
    uint32_t velocity;
    bool have_state;
    bool live;
    bool left_pressed;
    bool right_pressed;
    bool have_power_state = false;
    int32_t tz_offset_seconds;
    uint64_t tokens;
    example_progress_state_t progress = {0};
    m5_dualkey_board_power_state_t power_state = {0};
    example_status_doc_t doc = {0};
    cJSON *buddy_state = NULL;
    cJSON *entries = NULL;
    cJSON *prompt = NULL;
    cJSON *turn = NULL;
    cJSON *hw = NULL;
    cJSON *bat = NULL;

    (void)buddy;

    if (app->transport != NULL) {
        esp_desktop_buddy_transport_ble_get_state(app->transport, &transport_state);
    }

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    example_safe_copy(display_name, sizeof(display_name), app->display_name);
    example_safe_copy(owner_name, sizeof(owner_name), app->owner_name);
    example_safe_copy(last_turn_role, sizeof(last_turn_role), app->last_turn_role);
    state_cache = app->state_cache;
    approvals = app->approval_count;
    denials = app->denial_count;
    last_turn_len = app->last_turn_len;
    have_state = app->have_state;
    live = app->live;
    left_pressed = app->left_key_pressed;
    right_pressed = app->right_key_pressed;
    tz_offset_seconds = app->tz_offset_seconds;
    tokens = app->state_cache.tokens;
    progress = app->progress;
    xSemaphoreGive(app->mutex);

    level = example_progress_level(tokens);
    velocity = example_progress_velocity(&progress);
    have_power_state = m5_dualkey_board_get_power_state(&app->board, &power_state) == ESP_OK;

    if (example_status_begin(&doc,
                               display_name,
                               owner_name,
                               transport_state.encrypted,
                               approvals,
                               denials) != ESP_OK) {
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_begin_failed");
    }

    buddy_state = cJSON_AddObjectToObject(doc.root, "buddy");
    entries = cJSON_AddArrayToObject(buddy_state, "entries");
    prompt = cJSON_AddObjectToObject(buddy_state, "prompt");
    turn = cJSON_AddObjectToObject(buddy_state, "turn");
    hw = cJSON_AddObjectToObject(doc.root, "hw");
    if (have_power_state) {
        bat = cJSON_AddObjectToObject(doc.root, "bat");
    }
    if (buddy_state == NULL || entries == NULL || prompt == NULL || turn == NULL || hw == NULL ||
        (have_power_state && bat == NULL)) {
        cJSON_Delete(doc.root);
        return esp_desktop_buddy_status_err(ESP_FAIL, "status_section_alloc_failed");
    }

    cJSON_AddNumberToObject(doc.stats, "vel", (double)velocity);
    cJSON_AddNumberToObject(doc.stats, "nap", 0);
    cJSON_AddNumberToObject(doc.stats, "lvl", (double)level);
    cJSON_AddBoolToObject(buddy_state, "haveState", have_state);
    cJSON_AddBoolToObject(buddy_state, "live", live);
    cJSON_AddNumberToObject(buddy_state, "tz", (double)tz_offset_seconds);
    cJSON_AddNumberToObject(buddy_state, "total", (double)state_cache.total);
    cJSON_AddNumberToObject(buddy_state, "running", (double)state_cache.running);
    cJSON_AddNumberToObject(buddy_state, "waiting", (double)state_cache.waiting);
    cJSON_AddNumberToObject(buddy_state, "tokens", (double)state_cache.tokens);
    cJSON_AddNumberToObject(buddy_state, "tokensToday", (double)state_cache.tokens_today);
    cJSON_AddStringToObject(buddy_state, "msg", state_cache.msg);
    for (size_t i = 0; i < state_cache.entry_count; ++i) {
        cJSON_AddItemToArray(entries, cJSON_CreateString(state_cache.entries[i]));
    }

    cJSON_AddBoolToObject(prompt, "present", state_cache.prompt.present);
    cJSON_AddStringToObject(prompt, "id", state_cache.prompt.id);
    cJSON_AddStringToObject(prompt, "tool", state_cache.prompt.tool);
    cJSON_AddStringToObject(prompt, "hint", state_cache.prompt.hint);

    cJSON_AddStringToObject(turn, "role", last_turn_role);
    cJSON_AddNumberToObject(turn, "len", (double)last_turn_len);

    cJSON_AddStringToObject(hw, "board", "M5Stack Chain DualKey");
    cJSON_AddBoolToObject(hw, "leftPressed", left_pressed);
    cJSON_AddBoolToObject(hw, "rightPressed", right_pressed);
    cJSON_AddStringToObject(hw, "lhsAction", "deny");
    cJSON_AddStringToObject(hw, "rhsAction", "once");
    cJSON_AddStringToObject(hw,
                            "attention",
                            m5_dualkey_attention_label(&state_cache, &transport_state));
    if (transport_state.has_passkey) {
        cJSON_AddNumberToObject(hw, "passkey", (double)transport_state.passkey);
    }
    if (bat != NULL) {
        cJSON_AddNumberToObject(bat, "pct", (double)power_state.battery_pct);
        cJSON_AddNumberToObject(bat, "mV", (double)power_state.battery_mv);
        cJSON_AddNumberToObject(bat, "mA", power_state.charging ? -1 : 0);
        cJSON_AddBoolToObject(bat, "usb", power_state.usb_present);
    }

    return example_status_from_json(doc.root, status_json, sizeof(status_json));
}

esp_desktop_buddy_command_result_t m5_dualkey_name_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;

    (void)buddy;
    return example_update_persisted_string(app->mutex,
                                             app->display_name,
                                             sizeof(app->display_name),
                                             name,
                                             M5_DUALKEY_NVS_NAMESPACE,
                                             "display_name");
}

esp_desktop_buddy_command_result_t m5_dualkey_owner_handler(void *ctx, esp_desktop_buddy_t *buddy, const char *name)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;

    (void)buddy;

    return example_update_string_field(app->mutex,
                                         app->owner_name,
                                         sizeof(app->owner_name),
                                         name);
}

esp_desktop_buddy_command_result_t m5_dualkey_unpair_handler(void *ctx, esp_desktop_buddy_t *buddy)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;

    (void)buddy;
    return example_clear_bonds(app->transport);
}

void m5_dualkey_buddy_event(void *ctx, const esp_desktop_buddy_event_t *event)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;

    if (app == NULL || event == NULL) {
        return;
    }

    switch (event->type) {
    case ESP_DESKTOP_BUDDY_EVENT_SNAPSHOT_UPDATED: {
        bool had_prompt;
        example_buddy_state_cache_t previous_state = {0};
        example_buddy_state_cache_t state_cache = {0};

        if (example_buddy_state_cache_refresh(app->buddy, &state_cache) != ESP_OK) {
            break;
        }

        xSemaphoreTake(app->mutex, portMAX_DELAY);
        had_prompt = app->state_cache.prompt.present;
        previous_state = app->state_cache;
        example_progress_note_snapshot(&app->progress, &previous_state, &state_cache);
        app->state_cache = state_cache;
        app->have_state = state_cache.has_state;
        xSemaphoreGive(app->mutex);

        ESP_LOGI(TAG,
                 "state running=%lu waiting=%lu msg=%s",
                 (unsigned long)state_cache.running,
                 (unsigned long)state_cache.waiting,
                 state_cache.msg);
        if (!had_prompt && state_cache.prompt.present) {
            ESP_LOGI(TAG,
                     "attention: prompt id=%s tool=%s, lhs=deny rhs=accept-once",
                     state_cache.prompt.id,
                     state_cache.prompt.tool);
        } else if (had_prompt && !state_cache.prompt.present) {
            ESP_LOGI(TAG, "prompt cleared");
        }
        break;
    }
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
        if (example_apply_time_sync(app->mutex,
                                      &app->tz_offset_seconds,
                                      &event->data.time_sync) != ESP_OK) {
            ESP_LOGW(TAG, "settimeofday failed");
        }
        break;
    case ESP_DESKTOP_BUDDY_EVENT_TURN:
        xSemaphoreTake(app->mutex, portMAX_DELAY);
        app->turn_count++;
        app->last_turn_len = (uint32_t)event->data.turn.content.len;
        example_safe_copy(app->last_turn_role,
                            sizeof(app->last_turn_role),
                            event->data.turn.role);
        xSemaphoreGive(app->mutex);
        ESP_LOGI(TAG,
                 "turn role=%s len=%lu",
                 event->data.turn.role[0] ? event->data.turn.role : "<unknown>",
                 (unsigned long)event->data.turn.content.len);
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
        ESP_LOGW(TAG,
                 "buddy error kind=%d detail=%lu",
                 event->data.error.kind,
                 (unsigned long)event->data.error.detail);
        break;
    default:
        break;
    }
}

void m5_dualkey_transport_event(void *ctx, const esp_desktop_buddy_transport_ble_event_t *event)
{
    m5_dualkey_app_t *app = (m5_dualkey_app_t *)ctx;

    if (app == NULL || event == NULL) {
        return;
    }

    example_update_transport_state(app->mutex,
                                     &app->transport_state,
                                     app->owner_name,
                                     sizeof(app->owner_name),
                                     event);

    if ((event->changed_fields & ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_CONNECTED) != 0) {
        ESP_LOGI(TAG, "transport connected=%d", event->state.connected);
    }
    if ((event->changed_fields & ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_TX_READY) != 0) {
        ESP_LOGI(TAG, "transport tx_ready=%d", event->state.tx_ready);
    }
    if ((event->changed_fields & ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_PASSKEY) != 0) {
        if (event->state.has_passkey) {
            ESP_LOGI(TAG,
                     "PAIRING PASSKEY: %06lu (enter it on the central device)",
                     (unsigned long)event->state.passkey);
        } else {
            ESP_LOGI(TAG, "pairing passkey cleared");
        }
    }
}

void m5_dualkey_print_state(m5_dualkey_app_t *app, FILE *out)
{
    example_buddy_state_cache_t state_cache = {0};
    esp_desktop_buddy_transport_ble_state_t transport_state = {0};
    char display_name[M5_DUALKEY_NAME_MAX];
    char owner_name[M5_DUALKEY_OWNER_MAX];
    uint32_t approvals;
    uint32_t denials;
    bool have_state;
    bool left_pressed;
    bool right_pressed;

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    state_cache = app->state_cache;
    transport_state = app->transport_state;
    have_state = app->have_state;
    approvals = app->approval_count;
    denials = app->denial_count;
    left_pressed = app->left_key_pressed;
    right_pressed = app->right_key_pressed;
    example_safe_copy(display_name, sizeof(display_name), app->display_name);
    example_safe_copy(owner_name, sizeof(owner_name), app->owner_name);
    xSemaphoreGive(app->mutex);

    fprintf(out,
            "display=%s owner=%s transport[connected=%d subscribed=%d encrypted=%d bonded=%d tx_ready=%d]\n",
            display_name,
            owner_name,
            transport_state.connected,
            transport_state.subscribed,
            transport_state.encrypted,
            transport_state.bonded,
            transport_state.tx_ready);
    fprintf(out,
            "approvals=%lu denials=%lu buttons[left=%d right=%d lhs=deny rhs=once]\n",
            (unsigned long)approvals,
            (unsigned long)denials,
            left_pressed,
            right_pressed);
    fprintf(out,
            "attention=%s\n",
            m5_dualkey_attention_label(&state_cache, &transport_state));
    if (transport_state.has_passkey) {
        fprintf(out, "pairing passkey=%06lu\n", (unsigned long)transport_state.passkey);
    }

    if (!have_state) {
        fprintf(out, "state=<none>\n");
        fflush(out);
        return;
    }

    fprintf(out,
            "state total=%lu running=%lu waiting=%lu tokens=%llu today=%llu\n",
            (unsigned long)state_cache.total,
            (unsigned long)state_cache.running,
            (unsigned long)state_cache.waiting,
            (unsigned long long)state_cache.tokens,
            (unsigned long long)state_cache.tokens_today);
    fprintf(out, "msg=%s\n", state_cache.msg);
    if (state_cache.prompt.present) {
        fprintf(out, "prompt id=%s tool=%s hint=%s\n",
                state_cache.prompt.id,
                state_cache.prompt.tool,
                state_cache.prompt.hint);
    } else {
        fprintf(out, "prompt=<none>\n");
    }
    for (size_t i = 0; i < state_cache.entry_count; ++i) {
        fprintf(out, "entry[%lu]=%s\n", (unsigned long)i, state_cache.entries[i]);
    }
    fflush(out);
}
