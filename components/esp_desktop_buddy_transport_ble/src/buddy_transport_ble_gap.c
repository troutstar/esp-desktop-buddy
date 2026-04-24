/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "buddy_transport_ble_internal.h"

static const char *TAG = "esp_desktop_buddy_transport_ble";

static const ble_uuid128_t BUDDY_NUS_SERVICE_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static int esp_desktop_buddy_transport_ble_gap_event(struct ble_gap_event *event, void *arg);

static void esp_desktop_buddy_transport_ble_start_advertising(esp_desktop_buddy_transport_ble_t *transport)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    int rc;

    if (transport == NULL || transport->shutting_down) {
        return;
    }

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128 = (ble_uuid128_t[]){ BUDDY_NUS_SERVICE_UUID };
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
        return;
    }

    rsp_fields.name = (uint8_t *)transport->advertising_name;
    rsp_fields.name_len = strlen(transport->advertising_name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = CONFIG_ESP_DESKTOP_BUDDY_TRANSPORT_BLE_ADV_ITVL_MIN;
    adv_params.itvl_max = CONFIG_ESP_DESKTOP_BUDDY_TRANSPORT_BLE_ADV_ITVL_MAX;

    rc = ble_gap_adv_start(transport->own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           esp_desktop_buddy_transport_ble_gap_event,
                           transport);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "advertising as %s", transport->advertising_name);
}

static int esp_desktop_buddy_transport_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    esp_desktop_buddy_transport_ble_t *transport =
        arg != NULL ? (esp_desktop_buddy_transport_ble_t *)arg : g_esp_desktop_buddy_transport_ble_active;
    esp_desktop_buddy_transport_ble_state_t before;
    esp_desktop_buddy_transport_ble_state_t after;
    struct ble_gap_conn_desc desc;
    uint16_t mtu;
    bool was_tx_ready;
    int rc;

    if (transport == NULL) {
        return 0;
    }

    was_tx_ready = esp_desktop_buddy_transport_ble_is_tx_ready(transport);

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            bool same_conn;

            xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
            before = transport->state;
            same_conn = transport->conn_handle == event->connect.conn_handle;
            transport->conn_handle = event->connect.conn_handle;
            transport->mtu = BLE_ATT_MTU_DFLT;
            transport->state.connected = true;
            if (!same_conn) {
                transport->state.subscribed = false;
                transport->state.has_passkey = false;
                transport->state.passkey = 0;
            }
            esp_desktop_buddy_transport_ble_refresh_security_locked(transport);
            transport->state.tx_ready = esp_desktop_buddy_transport_ble_state_tx_ready(&transport->state);
            after = transport->state;
            xSemaphoreGive(transport->state_mutex);
            esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);

            ESP_LOGI(TAG, "ble connected conn_handle=%u", transport->conn_handle);
            if (!after.encrypted) {
                rc = ble_gap_security_initiate(transport->conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "ble_gap_security_initiate failed rc=%d", rc);
                }
            }
        } else if (!transport->shutting_down) {
            ESP_LOGW(TAG,
                     "connection attempt failed status=%d",
                     event->connect.status);
            esp_desktop_buddy_transport_ble_start_advertising(transport);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ble disconnected reason=%d", event->disconnect.reason);
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        before = transport->state;
        esp_desktop_buddy_transport_ble_reset_link_locked(transport);
        transport->state.tx_ready = false;
        after = transport->state;
        xSemaphoreGive(transport->state_mutex);
        esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);
        if (transport->buddy != NULL) {
            esp_desktop_buddy_transport_port_drop_frames(transport->buddy);
        }
        if (!transport->shutting_down) {
            esp_desktop_buddy_transport_ble_start_advertising(transport);
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE: {
        bool connected;

        if (!transport->shutting_down) {
            ESP_LOGI(TAG, "advertising complete reason=%d", event->adv_complete.reason);
            xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
            connected = transport->state.connected;
            xSemaphoreGive(transport->state_mutex);
            if (!connected) {
                esp_desktop_buddy_transport_ble_start_advertising(transport);
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle != transport->tx_handle) {
            return 0;
        }
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        before = transport->state;
        transport->conn_handle = event->subscribe.conn_handle;
        transport->state.connected = true;
        esp_desktop_buddy_transport_ble_refresh_security_locked(transport);
        transport->state.subscribed = event->subscribe.cur_notify != 0;
        transport->state.tx_ready = esp_desktop_buddy_transport_ble_state_tx_ready(&transport->state);
        after = transport->state;
        xSemaphoreGive(transport->state_mutex);
        esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);
        ESP_LOGI(TAG, "tx subscribe notify=%d", after.subscribed);
        if (was_tx_ready && !after.tx_ready && transport->buddy != NULL) {
            esp_desktop_buddy_transport_port_drop_frames(transport->buddy);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        mtu = 0;
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        if (event->mtu.conn_handle == transport->conn_handle) {
            transport->mtu = event->mtu.value;
            mtu = transport->mtu;
        }
        xSemaphoreGive(transport->state_mutex);
        if (mtu != 0) {
            ESP_LOGI(TAG, "mtu updated to %u", mtu);
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        before = transport->state;
        transport->conn_handle = event->enc_change.conn_handle;
        transport->state.connected = true;
        esp_desktop_buddy_transport_ble_refresh_security_locked(transport);
        if (event->enc_change.status == 0) {
            transport->state.encrypted = true;
            transport->state.has_passkey = false;
            transport->state.passkey = 0;
        }
        transport->state.tx_ready = esp_desktop_buddy_transport_ble_state_tx_ready(&transport->state);
        after = transport->state;
        xSemaphoreGive(transport->state_mutex);
        esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);
        if (was_tx_ready && !after.tx_ready && transport->buddy != NULL) {
            esp_desktop_buddy_transport_port_drop_frames(transport->buddy);
        }
        ESP_LOGI(TAG,
                 "encryption change status=%d encrypted=%d bonded=%d",
                 event->enc_change.status,
                 after.encrypted,
                 after.bonded);
        if (event->enc_change.status != 0 && !transport->shutting_down) {
            rc = ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (rc != 0 && rc != BLE_HS_ENOTCONN) {
                ESP_LOGW(TAG,
                         "ble_gap_terminate failed rc=%d after enc_change status=%d",
                         rc,
                         event->enc_change.status);
            }
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            if (rc != 0) {
                ESP_LOGW(TAG, "ble_store_util_delete_peer failed rc=%d", rc);
            } else {
                ESP_LOGI(TAG, "deleted old bond for repeat pairing");
            }
        } else {
            ESP_LOGW(TAG, "ble_gap_conn_find failed rc=%d during repeat pairing", rc);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};

        if (event->passkey.params.action != BLE_SM_IOACT_DISP) {
            ESP_LOGW(TAG, "unsupported passkey action=%d", event->passkey.params.action);
            return 0;
        }

        pkey.action = event->passkey.params.action;
        pkey.passkey = esp_random() % 1000000;
        rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_sm_inject_io failed rc=%d", rc);
            return 0;
        }

        xSemaphoreTake(transport->state_mutex, portMAX_DELAY);
        before = transport->state;
        transport->state.has_passkey = true;
        transport->state.passkey = pkey.passkey;
        transport->state.tx_ready = esp_desktop_buddy_transport_ble_state_tx_ready(&transport->state);
        after = transport->state;
        xSemaphoreGive(transport->state_mutex);
        esp_desktop_buddy_transport_ble_emit_state_change(transport, before, after);
        ESP_LOGI(TAG, "pairing passkey %06lu", (unsigned long)pkey.passkey);
        return 0;
    }
    default:
        return 0;
    }
}

void esp_desktop_buddy_transport_ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset reason=%d", reason);
}

void esp_desktop_buddy_transport_ble_on_sync(void)
{
    uint8_t addr_val[6] = {0};
    int rc;

    if (g_esp_desktop_buddy_transport_ble_active == NULL) {
        return;
    }

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &g_esp_desktop_buddy_transport_ble_active->own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_copy_addr(g_esp_desktop_buddy_transport_ble_active->own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_copy_addr failed rc=%d", rc);
        return;
    }

    rc = ble_svc_gap_device_name_set(g_esp_desktop_buddy_transport_ble_active->advertising_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG,
             "ble sync complete addr=%02x:%02x:%02x:%02x:%02x:%02x",
             addr_val[0],
             addr_val[1],
             addr_val[2],
             addr_val[3],
             addr_val[4],
             addr_val[5]);
    esp_desktop_buddy_transport_ble_start_advertising(g_esp_desktop_buddy_transport_ble_active);
}

void esp_desktop_buddy_transport_ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
    if (g_esp_desktop_buddy_transport_ble_active != NULL &&
        g_esp_desktop_buddy_transport_ble_active->host_stop_sem != NULL) {
        xSemaphoreGive(g_esp_desktop_buddy_transport_ble_active->host_stop_sem);
    }
    vTaskDelete(NULL);
}
