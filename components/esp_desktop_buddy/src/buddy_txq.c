/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include "buddy_internal.h"

esp_err_t esp_desktop_buddy_txq_enqueue_frame(esp_desktop_buddy_t *buddy, const uint8_t *bytes, size_t len)
{
    esp_desktop_buddy_tx_frame_t *frame = NULL;
    esp_err_t err = ESP_OK;

    if (buddy == NULL || bytes == NULL || len == 0 || len > ESP_DESKTOP_BUDDY_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    frame = calloc(1, sizeof(*frame));
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    frame->len = len;
    memcpy(frame->bytes, bytes, len);

    if (xQueueSend(buddy->tx_queue, frame, 0) != pdTRUE) {
        err = ESP_ERR_NO_MEM;
    }

    free(frame);
    return err;
}

esp_err_t esp_desktop_buddy_transport_port_next_frame(esp_desktop_buddy_t *buddy,
                           uint8_t *buf,
                           size_t buf_size,
                           size_t *out_len,
                           TickType_t timeout)
{
    esp_desktop_buddy_tx_frame_t *frame = NULL;
    esp_err_t err = ESP_OK;

    if (buddy == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;

    frame = malloc(sizeof(*frame));
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xQueuePeek(buddy->tx_queue, frame, timeout) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
        goto done;
    }

    *out_len = frame->len;
    if (frame->len > buf_size) {
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    if (xQueueReceive(buddy->tx_queue, frame, 0) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
        goto done;
    }

    memcpy(buf, frame->bytes, frame->len);

done:
    free(frame);
    return err;
}

void esp_desktop_buddy_transport_port_drop_frames(esp_desktop_buddy_t *buddy)
{
    if (buddy == NULL || buddy->tx_queue == NULL) {
        return;
    }

    xQueueReset(buddy->tx_queue);
}
