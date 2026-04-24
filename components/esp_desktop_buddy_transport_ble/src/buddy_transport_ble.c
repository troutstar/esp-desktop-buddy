/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "buddy_transport_ble_internal.h"

static const char *TAG = "esp_desktop_buddy_transport_ble";

esp_desktop_buddy_transport_ble_t *g_esp_desktop_buddy_transport_ble_active;

bool esp_desktop_buddy_transport_ble_state_tx_ready(const esp_desktop_buddy_transport_ble_state_t *state)
{
    return state->connected && state->subscribed && state->encrypted;
}

static uint8_t esp_desktop_buddy_transport_ble_map_io_cap(esp_desktop_buddy_transport_ble_io_cap_t io_capability)
{
    switch (io_capability) {
    case ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY:
        return BLE_SM_IO_CAP_DISP_ONLY;
    case ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_NO_INPUT_OUTPUT:
    default:
        return BLE_SM_IO_CAP_NO_IO;
    }
}

static bool esp_desktop_buddy_transport_ble_supports_io_capability(
    esp_desktop_buddy_transport_ble_io_cap_t io_capability)
{
    switch (io_capability) {
    case ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_DISPLAY_ONLY:
    case ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_NO_INPUT_OUTPUT:
        return true;
    default:
        return false;
    }
}

void esp_desktop_buddy_transport_ble_emit_state_change(esp_desktop_buddy_transport_ble_t *transport,
                                           esp_desktop_buddy_transport_ble_state_t before,
                                           esp_desktop_buddy_transport_ble_state_t after)
{
    esp_desktop_buddy_transport_ble_event_t event = {
        .state = after,
    };

    if (before.connected != after.connected) {
        event.changed_fields |= ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_CONNECTED;
    }
    if (before.subscribed != after.subscribed) {
        event.changed_fields |= ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_SUBSCRIBED;
    }
    if (before.encrypted != after.encrypted) {
        event.changed_fields |= ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_ENCRYPTED;
    }
    if (before.bonded != after.bonded) {
        event.changed_fields |= ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_BONDED;
    }
    if (before.tx_ready != after.tx_ready) {
        event.changed_fields |= ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_TX_READY;
    }
    if (before.has_passkey != after.has_passkey ||
        (after.has_passkey && before.passkey != after.passkey)) {
        event.changed_fields |= ESP_DESKTOP_BUDDY_TRANSPORT_BLE_FIELD_PASSKEY;
    }

    if (event.changed_fields == 0 || transport->on_event == NULL) {
        return;
    }

    transport->on_event(transport->event_ctx, &event);
}

static void esp_desktop_buddy_transport_ble_build_default_name(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};

    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }

    snprintf(out, out_size, "Claude-%02X%02X", mac[4], mac[5]);
}

void esp_desktop_buddy_transport_ble_reset_link_locked(esp_desktop_buddy_transport_ble_t *transport)
{
    transport->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    transport->mtu = BLE_ATT_MTU_DFLT;
    transport->state.connected = false;
    transport->state.subscribed = false;
    transport->state.encrypted = false;
    transport->state.bonded = false;
    transport->state.has_passkey = false;
    transport->state.passkey = 0;
}

void esp_desktop_buddy_transport_ble_refresh_security_locked(esp_desktop_buddy_transport_ble_t *transport)
{
    struct ble_gap_conn_desc desc;

    if (transport->conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_conn_find(transport->conn_handle, &desc) != 0) {
        transport->state.encrypted = false;
        transport->state.bonded = false;
        return;
    }

    transport->state.encrypted = desc.sec_state.encrypted != 0;
    transport->state.bonded = desc.sec_state.bonded != 0;
}

void esp_desktop_buddy_transport_ble_force_link_recovery(esp_desktop_buddy_transport_ble_t *transport)
{
    esp_desktop_buddy_transport_ble_state_t before;
    esp_desktop_buddy_transport_ble_state_t after;
    int rc;

    if (transport == NULL) {
        return;
    }

    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    before = transport->state;
    transport->state.tx_ready = false;
    after = transport->state;
    xSemaphoreGive(transport->state_mutex);
    esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);

    if (transport->conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    rc = ble_gap_terminate(transport->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "ble_gap_terminate failed rc=%d after tx error", rc);
    }
}

uint16_t esp_desktop_buddy_transport_ble_get_mtu(esp_desktop_buddy_transport_ble_t *transport)
{
    uint16_t mtu = BLE_ATT_MTU_DFLT;

    if (transport == NULL || transport->state_mutex == NULL) {
        return mtu;
    }

    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    mtu = transport->mtu;
    xSemaphoreGive(transport->state_mutex);
    return mtu;
}

esp_err_t esp_desktop_buddy_transport_ble_wait_for_stop(SemaphoreHandle_t stop_sem,
                                            TickType_t timeout,
                                            const char *task_name)
{
    if (stop_sem == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(stop_sem, timeout) == pdTRUE) {
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "%s did not stop before timeout; leaving transport allocated to avoid use-after-free",
             task_name);
    return ESP_ERR_TIMEOUT;
}

void esp_desktop_buddy_transport_ble_delete_unstarted(esp_desktop_buddy_transport_ble_t *transport,
                                          bool nimble_initialized)
{
    if (transport == NULL) {
        return;
    }

    transport->shutting_down = true;
    if (transport->buddy_attached && transport->buddy != NULL) {
        esp_desktop_buddy_transport_port_detach(transport->buddy);
        transport->buddy_attached = false;
    }
    if (g_esp_desktop_buddy_transport_ble_active == transport) {
        g_esp_desktop_buddy_transport_ble_active = NULL;
    }
    if (nimble_initialized) {
        nimble_port_deinit();
    }
    if (transport->state_mutex != NULL) {
        vSemaphoreDelete(transport->state_mutex);
    }
    if (transport->host_stop_sem != NULL) {
        vSemaphoreDelete(transport->host_stop_sem);
    }
    if (transport->tx_stop_sem != NULL) {
        vSemaphoreDelete(transport->tx_stop_sem);
    }
    free(transport);
}

esp_err_t esp_desktop_buddy_transport_ble_new(const esp_desktop_buddy_transport_ble_config_t *config,
                                  esp_desktop_buddy_transport_ble_t **out_transport)
{
    esp_desktop_buddy_transport_ble_t *transport;
    int rc;
    esp_err_t err;

    if (config == NULL || out_transport == NULL || config->buddy == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_desktop_buddy_transport_ble_supports_io_capability(config->security.io_capability)) {
        ESP_LOGE(TAG, "unsupported io_capability=%d for esp_desktop_buddy_transport_ble",
                 (int)config->security.io_capability);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (config->security.mitm &&
        config->security.io_capability == ESP_DESKTOP_BUDDY_TRANSPORT_BLE_IO_CAP_NO_INPUT_OUTPUT) {
        ESP_LOGE(TAG, "mitm requires an interactive io_capability");
        return ESP_ERR_INVALID_ARG;
    }
    if (g_esp_desktop_buddy_transport_ble_active != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    transport = calloc(1, sizeof(*transport));
    if (transport == NULL) {
        return ESP_ERR_NO_MEM;
    }

    transport->buddy = config->buddy;
    transport->on_event = config->on_event;
    transport->event_ctx = config->event_ctx;
    transport->security = config->security;
    transport->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    transport->mtu = BLE_ATT_MTU_DFLT;
    transport->state_mutex = xSemaphoreCreateMutex();
    transport->host_stop_sem = xSemaphoreCreateBinary();
    transport->tx_stop_sem = xSemaphoreCreateBinary();
    if (transport->state_mutex == NULL ||
        transport->host_stop_sem == NULL ||
        transport->tx_stop_sem == NULL) {
        esp_desktop_buddy_transport_ble_delete_unstarted(transport, false);
        return ESP_ERR_NO_MEM;
    }

    if (config->advertising_name_override != NULL &&
        config->advertising_name_override[0] != '\0') {
        strlcpy(transport->advertising_name,
                config->advertising_name_override,
                sizeof(transport->advertising_name));
    } else {
        esp_desktop_buddy_transport_ble_build_default_name(transport->advertising_name,
                                               sizeof(transport->advertising_name));
    }

    esp_desktop_buddy_transport_ble_prepare_gatt(transport);

    err = esp_desktop_buddy_transport_port_attach(transport->buddy);
    if (err != ESP_OK) {
        esp_desktop_buddy_transport_ble_delete_unstarted(transport, false);
        return err;
    }
    transport->buddy_attached = true;
    g_esp_desktop_buddy_transport_ble_active = transport;

    err = nimble_port_init();
    if (err != ESP_OK) {
        esp_desktop_buddy_transport_ble_delete_unstarted(transport, false);
        return err;
    }

    ble_hs_cfg.reset_cb = esp_desktop_buddy_transport_ble_on_reset;
    ble_hs_cfg.sync_cb = esp_desktop_buddy_transport_ble_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_desktop_buddy_transport_ble_gatt_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = esp_desktop_buddy_transport_ble_map_io_cap(transport->security.io_capability);
    ble_hs_cfg.sm_bonding = transport->security.bonding ? 1 : 0;
    ble_hs_cfg.sm_sc = transport->security.secure_connections ? 1 : 0;
    ble_hs_cfg.sm_mitm = transport->security.mitm ? 1 : 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();

    if (transport->security.bonding && !CONFIG_BT_NIMBLE_NVS_PERSIST) {
        ESP_LOGW(TAG,
                 "bonding enabled without CONFIG_BT_NIMBLE_NVS_PERSIST; bonds will not survive reboot");
    }
    if (CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 1) {
        ESP_LOGW(TAG,
                 "transport is designed for one active BLE peer; advertising stays off while connected");
    }

    rc = ble_gatts_count_cfg(transport->gatt_svcs);
    if (rc != 0) {
        esp_desktop_buddy_transport_ble_delete_unstarted(transport, true);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(transport->gatt_svcs);
    if (rc != 0) {
        esp_desktop_buddy_transport_ble_delete_unstarted(transport, true);
        return ESP_FAIL;
    }

    if (xTaskCreate(esp_desktop_buddy_transport_ble_tx_task,
                    "buddy_nus_tx",
                    ESP_DESKTOP_BUDDY_TRANSPORT_BLE_TX_TASK_STACK,
                    transport,
                    ESP_DESKTOP_BUDDY_TRANSPORT_BLE_TX_TASK_PRIORITY,
                    &transport->tx_task) != pdPASS) {
        esp_desktop_buddy_transport_ble_delete_unstarted(transport, true);
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(esp_desktop_buddy_transport_ble_host_task);

    *out_transport = transport;
    return ESP_OK;
}

esp_err_t esp_desktop_buddy_transport_ble_delete(esp_desktop_buddy_transport_ble_t *transport)
{
    esp_err_t err;
    int rc;

    if (transport == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    transport->shutting_down = true;
    if (transport->buddy != NULL) {
        esp_desktop_buddy_transport_port_drop_frames(transport->buddy);
    }

    if (transport->tx_task != NULL) {
        err = esp_desktop_buddy_transport_ble_wait_for_stop(transport->tx_stop_sem,
                                                pdMS_TO_TICKS(1000),
                                                "tx task");
        if (err != ESP_OK) {
            return err;
        }
        transport->tx_task = NULL;
    }

    if (g_esp_desktop_buddy_transport_ble_active == transport) {
        if (transport->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(transport->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        ble_gap_adv_stop();
        rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop failed rc=%d", rc);
            return ESP_FAIL;
        }
        err = esp_desktop_buddy_transport_ble_wait_for_stop(transport->host_stop_sem,
                                                pdMS_TO_TICKS(2000),
                                                "host task");
        if (err != ESP_OK) {
            return err;
        }
        nimble_port_deinit();
        g_esp_desktop_buddy_transport_ble_active = NULL;
    }

    if (transport->state_mutex != NULL) {
        vSemaphoreDelete(transport->state_mutex);
    }
    if (transport->host_stop_sem != NULL) {
        vSemaphoreDelete(transport->host_stop_sem);
    }
    if (transport->tx_stop_sem != NULL) {
        vSemaphoreDelete(transport->tx_stop_sem);
    }
    if (transport->buddy_attached && transport->buddy != NULL) {
        esp_desktop_buddy_transport_port_detach(transport->buddy);
        transport->buddy_attached = false;
    }
    free(transport);
    return ESP_OK;
}

esp_err_t esp_desktop_buddy_transport_ble_clear_bonds(esp_desktop_buddy_transport_ble_t *transport)
{
    esp_desktop_buddy_transport_ble_state_t before;
    esp_desktop_buddy_transport_ble_state_t after;
    uint16_t conn_handle;
    int rc;

    if (transport == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "clearing local BLE bonds");
    rc = ble_store_clear();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_store_clear failed rc=%d", rc);
        return ESP_FAIL;
    }

    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    before = transport->state;
    conn_handle = transport->conn_handle;
    transport->state.bonded = false;
    transport->state.has_passkey = false;
    transport->state.passkey = 0;
    transport->state.tx_ready = esp_desktop_buddy_transport_ble_state_tx_ready(&transport->state);
    after = transport->state;
    xSemaphoreGive(transport->state_mutex);

    esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "terminating active BLE link after bond clear conn_handle=%u", conn_handle);
        rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG,
                     "local bonds cleared but ble_gap_terminate failed rc=%d",
                     rc);
        }
    }
    ESP_LOGI(TAG, "local BLE bonds cleared");
    return ESP_OK;
}

esp_err_t esp_desktop_buddy_transport_ble_get_state(esp_desktop_buddy_transport_ble_t *transport,
                                        esp_desktop_buddy_transport_ble_state_t *out_state)
{
    if (transport == NULL || out_state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
    *out_state = transport->state;
    xSemaphoreGive(transport->state_mutex);
    return ESP_OK;
}

esp_err_t esp_desktop_buddy_transport_ble_get_advertising_name(esp_desktop_buddy_transport_ble_t *transport,
                                                    char *out_name,
                                                    size_t out_name_size)
{
    if (transport == NULL || out_name == NULL || out_name_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(out_name, transport->advertising_name, out_name_size);
    return ESP_OK;
}

bool esp_desktop_buddy_transport_ble_is_tx_ready(esp_desktop_buddy_transport_ble_t *transport)
{
    esp_desktop_buddy_transport_ble_state_t state = {0};

    if (esp_desktop_buddy_transport_ble_get_state(transport, &state) != ESP_OK) {
        return false;
    }

    return state.tx_ready;
}
