/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "wear_levelling.h"

#include "example_charpack_internal.h"

static const char *TAG = "example_charpack";
static const char *EXAMPLE_CHARPACK_ACTIVE_FILENAME = "active_pack";

static void example_charpack_safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
}

static bool example_charpack_info_matches(const example_charpack_info_t *lhs,
                                          const example_charpack_info_t *rhs)
{
    return lhs != NULL &&
           rhs != NULL &&
           lhs->mode == rhs->mode &&
           strcmp(lhs->pack_id, rhs->pack_id) == 0;
}

static bool example_charpack_is_dir(const char *path)
{
    struct stat st;

    if (path == NULL || stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static esp_err_t example_charpack_ensure_dir(const char *path)
{
    char buf[EXAMPLE_CHARPACK_PATH_MAX];
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(buf, path, sizeof(buf));
    len = strlen(buf);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = '\0';
        if (!example_charpack_is_dir(buf) && mkdir(buf, 0777) != 0 && errno != EEXIST) {
            return ESP_FAIL;
        }
        buf[i] = '/';
    }

    if (!example_charpack_is_dir(buf) && mkdir(buf, 0777) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void example_charpack_remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(path);
    if (dir == NULL) {
        unlink(path);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char full[EXAMPLE_CHARPACK_PATH_MAX];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(full, sizeof(full), "%s/%s", path, entry->d_name) >= (int)sizeof(full)) {
            continue;
        }
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            example_charpack_remove_tree(full);
            rmdir(full);
        } else {
            unlink(full);
        }
    }

    closedir(dir);
}

static void example_charpack_remove_path(const char *path)
{
    struct stat st;

    if (path == NULL || stat(path, &st) != 0) {
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        example_charpack_remove_tree(path);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static void example_charpack_clear_transfer_locked(example_charpack_t *charpack)
{
    if (charpack == NULL) {
        return;
    }

    if (charpack->transfer.file != NULL) {
        fclose(charpack->transfer.file);
        charpack->transfer.file = NULL;
    }
    memset(&charpack->transfer, 0, sizeof(charpack->transfer));
}

static esp_err_t example_charpack_close_file(FILE **file, bool flush)
{
    int flush_rc = 0;
    int close_rc;

    if (file == NULL || *file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (flush) {
        flush_rc = fflush(*file);
    }
    close_rc = fclose(*file);
    *file = NULL;

    if ((flush && flush_rc != 0) || close_rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t example_charpack_write_active_file(example_charpack_t *charpack,
                                                      const char *pack_id)
{
    char tmp_path[EXAMPLE_CHARPACK_PATH_MAX];
    FILE *file;

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", charpack->active_path) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    if (fwrite(pack_id, 1, strlen(pack_id), file) != strlen(pack_id)) {
        (void)example_charpack_close_file(&file, false);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (example_charpack_close_file(&file, true) != ESP_OK) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    // FAT-backed rename does not replace an existing target file in place.
    if (unlink(charpack->active_path) != 0 && errno != ENOENT) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    if (rename(tmp_path, charpack->active_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void example_charpack_clear_active_locked(example_charpack_t *charpack, bool emit_event)
{
    bool had_active = charpack->active_present;

    charpack->active_present = false;
    memset(&charpack->active_info, 0, sizeof(charpack->active_info));
    unlink(charpack->active_path);

    if (emit_event && had_active) {
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_ACTIVE_CLEARED,
                                      NULL,
                                      NULL,
                                      0,
                                      0,
                                      0);
    }
}

static esp_err_t example_charpack_validate_installed_pack(example_charpack_t *charpack,
                                                            const char *pack_id,
                                                            example_charpack_info_t *out_info)
{
    char manifest_path[EXAMPLE_CHARPACK_PATH_MAX];
    const char *error_token = NULL;

    if (!example_charpack_is_safe_pack_id(pack_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(manifest_path,
                 sizeof(manifest_path),
                 "%s/%s/manifest.json",
                 charpack->packs_root,
                 pack_id) >= (int)sizeof(manifest_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return example_charpack_read_manifest_info(
        manifest_path, pack_id, NULL, out_info, &error_token);
}

static void example_charpack_refresh_active_from_disk(example_charpack_t *charpack)
{
    char pack_id[EXAMPLE_CHARPACK_PACK_ID_MAX + 1] = {0};
    FILE *file;
    example_charpack_info_t info = {0};
    size_t bytes_read;

    file = fopen(charpack->active_path, "rb");
    if (file == NULL) {
        charpack->active_present = false;
        memset(&charpack->active_info, 0, sizeof(charpack->active_info));
        return;
    }

    bytes_read = fread(pack_id, 1, sizeof(pack_id) - 1, file);
    if (bytes_read == 0) {
        fclose(file);
        example_charpack_clear_active_locked(charpack, false);
        return;
    }
    if (bytes_read == sizeof(pack_id) - 1 && fgetc(file) != EOF) {
        fclose(file);
        example_charpack_clear_active_locked(charpack, false);
        return;
    }
    fclose(file);

    if (example_charpack_validate_installed_pack(charpack, pack_id, &info) != ESP_OK) {
        example_charpack_clear_active_locked(charpack, false);
        return;
    }

    charpack->active_info = info;
    charpack->active_present = true;
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_sink_ok(void)
{
    return esp_desktop_buddy_folder_push_result_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_sink_storage_failed(const char *detail)
{
    return esp_desktop_buddy_folder_push_result_err(ESP_FAIL,
                                                   ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_STORAGE_FAILED,
                                                   detail);
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_sink_invalid_content(const char *detail)
{
    return esp_desktop_buddy_folder_push_result_err(ESP_ERR_INVALID_ARG,
                                                   ESP_DESKTOP_BUDDY_FOLDER_PUSH_SINK_REASON_INVALID_CONTENT,
                                                   detail);
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_begin_transfer(void *ctx,
                                                                    const char *name,
                                                                    uint32_t total_bytes)
{
    example_charpack_t *charpack = (example_charpack_t *)ctx;
    char pack_id[EXAMPLE_CHARPACK_PACK_ID_MAX + 1];
    char transfer_root[EXAMPLE_CHARPACK_PATH_MAX];

    if (!example_charpack_normalize_pack_id(name, pack_id, sizeof(pack_id))) {
        return example_charpack_sink_invalid_content("invalid_pack_id");
    }
    if (snprintf(transfer_root, sizeof(transfer_root), "%s/%s", charpack->staging_root, pack_id) >=
        (int)sizeof(transfer_root)) {
        return example_charpack_sink_storage_failed("staging_path_too_long");
    }

    example_charpack_remove_tree(transfer_root);
    if (example_charpack_ensure_dir(transfer_root) != ESP_OK) {
        return example_charpack_sink_storage_failed("staging_dir_create_failed");
    }

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    memset(&charpack->transfer, 0, sizeof(charpack->transfer));
    charpack->transfer.active = true;
    charpack->transfer.total_bytes = total_bytes;
    example_charpack_safe_copy(charpack->transfer.pack_id,
                                 sizeof(charpack->transfer.pack_id),
                                 pack_id);
    example_charpack_safe_copy(charpack->transfer.source_name,
                                 sizeof(charpack->transfer.source_name),
                                 name);
    xSemaphoreGive(charpack->mutex);

    example_charpack_info_t info = {0};
    example_charpack_safe_copy(info.pack_id, sizeof(info.pack_id), pack_id);
    info.mode = EXAMPLE_CHARPACK_MODE_GIF;
    example_charpack_emit_event(charpack,
                                  EXAMPLE_CHARPACK_EVENT_TRANSFER_STARTED,
                                  &info,
                                  NULL,
                                  0,
                                  0,
                                  total_bytes);
    return example_charpack_sink_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_begin_file(void *ctx,
                                                                const char *path,
                                                                uint32_t size)
{
    example_charpack_t *charpack = (example_charpack_t *)ctx;
    char full_path[EXAMPLE_CHARPACK_PATH_MAX];
    FILE *file;
    example_charpack_info_t info = {0};

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    if (!charpack->transfer.active) {
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("transfer_not_active");
    }
    if (snprintf(full_path,
                 sizeof(full_path),
                 "%s/%s/%s",
                 charpack->staging_root,
                 charpack->transfer.pack_id,
                 path) >= (int)sizeof(full_path)) {
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_path_too_long");
    }
    file = fopen(full_path, "wb");
    if (file == NULL) {
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_open_failed");
    }

    charpack->transfer.file = file;
    charpack->transfer.file_size = size;
    example_charpack_safe_copy(charpack->transfer.current_path,
                                 sizeof(charpack->transfer.current_path),
                                 path);
    example_charpack_safe_copy(info.pack_id, sizeof(info.pack_id), charpack->transfer.pack_id);
    xSemaphoreGive(charpack->mutex);

    example_charpack_emit_event(charpack,
                                  EXAMPLE_CHARPACK_EVENT_FILE_STARTED,
                                  &info,
                                  path,
                                  size,
                                  0,
                                  0);
    return example_charpack_sink_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_write_chunk(void *ctx,
                                                                 const uint8_t *data,
                                                                 size_t len)
{
    example_charpack_t *charpack = (example_charpack_t *)ctx;
    example_charpack_info_t info = {0};

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    if (!charpack->transfer.active || charpack->transfer.file == NULL) {
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_not_open");
    }
    if (fwrite(data, 1, len, charpack->transfer.file) != len) {
        (void)example_charpack_close_file(&charpack->transfer.file, false);
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_write_failed");
    }
    if (fflush(charpack->transfer.file) != 0) {
        (void)example_charpack_close_file(&charpack->transfer.file, false);
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_flush_failed");
    }
    charpack->transfer.bytes_written += (uint32_t)len;
    example_charpack_safe_copy(info.pack_id, sizeof(info.pack_id), charpack->transfer.pack_id);
    uint32_t bytes_written = charpack->transfer.bytes_written;
    uint32_t total_bytes = charpack->transfer.total_bytes;
    xSemaphoreGive(charpack->mutex);

    example_charpack_emit_event(charpack,
                                  EXAMPLE_CHARPACK_EVENT_TRANSFER_PROGRESS,
                                  &info,
                                  NULL,
                                  0,
                                  bytes_written,
                                  total_bytes);
    return example_charpack_sink_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_end_file(void *ctx)
{
    example_charpack_t *charpack = (example_charpack_t *)ctx;
    example_charpack_info_t info = {0};
    char path[EXAMPLE_CHARPACK_PATH_MAX];
    uint32_t size;

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    if (!charpack->transfer.active || charpack->transfer.file == NULL) {
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_not_open");
    }
    if (example_charpack_close_file(&charpack->transfer.file, true) != ESP_OK) {
        xSemaphoreGive(charpack->mutex);
        return example_charpack_sink_storage_failed("file_close_failed");
    }
    size = charpack->transfer.file_size;
    example_charpack_safe_copy(path, sizeof(path), charpack->transfer.current_path);
    charpack->transfer.current_path[0] = '\0';
    example_charpack_safe_copy(info.pack_id, sizeof(info.pack_id), charpack->transfer.pack_id);
    xSemaphoreGive(charpack->mutex);

    example_charpack_emit_event(charpack,
                                  EXAMPLE_CHARPACK_EVENT_FILE_FINISHED,
                                  &info,
                                  path,
                                  size,
                                  0,
                                  0);
    return example_charpack_sink_ok();
}

static esp_desktop_buddy_folder_push_sink_result_t example_charpack_end_transfer(void *ctx)
{
    example_charpack_t *charpack = (example_charpack_t *)ctx;
    char staging_path[EXAMPLE_CHARPACK_PATH_MAX];
    char manifest_path[EXAMPLE_CHARPACK_PATH_MAX];
    char dest_path[EXAMPLE_CHARPACK_PATH_MAX];
    char backup_path[EXAMPLE_CHARPACK_PATH_MAX];
    char expected_pack_id[EXAMPLE_CHARPACK_PACK_ID_MAX + 1];
    char expected_source_name[EXAMPLE_CHARPACK_PATH_MAX];
    const char *error_token = NULL;
    example_charpack_info_t info = {0};
    example_charpack_info_t previous_active = {0};
    bool had_previous_active = false;
    bool active_changed = false;
    bool moved_existing_dest = false;

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    example_charpack_safe_copy(info.pack_id, sizeof(info.pack_id), charpack->transfer.pack_id);
    example_charpack_safe_copy(expected_pack_id,
                                 sizeof(expected_pack_id),
                                 charpack->transfer.pack_id);
    example_charpack_safe_copy(expected_source_name,
                                 sizeof(expected_source_name),
                                 charpack->transfer.source_name);
    had_previous_active = charpack->active_present;
    previous_active = charpack->active_info;
    if (snprintf(staging_path,
                 sizeof(staging_path),
                 "%s/%s",
                 charpack->staging_root,
                 charpack->transfer.pack_id) >= (int)sizeof(staging_path) ||
        snprintf(manifest_path,
                 sizeof(manifest_path),
                 "%s/manifest.json",
                 staging_path) >= (int)sizeof(manifest_path) ||
        snprintf(dest_path,
                 sizeof(dest_path),
                 "%s/%s",
                 charpack->packs_root,
                 charpack->transfer.pack_id) >= (int)sizeof(dest_path) ||
        snprintf(backup_path,
                 sizeof(backup_path),
                 "%s/.rollback_%s",
                 charpack->mount_point,
                 charpack->transfer.pack_id) >= (int)sizeof(backup_path)) {
        example_charpack_clear_transfer_locked(charpack);
        xSemaphoreGive(charpack->mutex);
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_INSTALL_FAILED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
        return example_charpack_sink_storage_failed("backup_path_too_long");
    }
    xSemaphoreGive(charpack->mutex);

    if (example_charpack_read_manifest_info(manifest_path,
                                              expected_pack_id,
                                              expected_source_name,
                                              &info,
                                              &error_token) != ESP_OK) {
        example_charpack_remove_path(staging_path);
        xSemaphoreTake(charpack->mutex, portMAX_DELAY);
        example_charpack_clear_transfer_locked(charpack);
        xSemaphoreGive(charpack->mutex);
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_INSTALL_FAILED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
        return example_charpack_sink_invalid_content(error_token != NULL ? error_token : "invalid_manifest");
    }

    example_charpack_remove_path(backup_path);
    if (example_charpack_is_dir(dest_path)) {
        if (rename(dest_path, backup_path) != 0) {
            example_charpack_remove_path(staging_path);
            xSemaphoreTake(charpack->mutex, portMAX_DELAY);
            example_charpack_clear_transfer_locked(charpack);
            xSemaphoreGive(charpack->mutex);
            example_charpack_emit_event(charpack,
                                          EXAMPLE_CHARPACK_EVENT_INSTALL_FAILED,
                                          &info,
                                          NULL,
                                          0,
                                          0,
                                          0);
            return example_charpack_sink_storage_failed("backup_existing_pack_failed");
        }
        moved_existing_dest = true;
    }

    if (rename(staging_path, dest_path) != 0) {
        example_charpack_remove_path(staging_path);
        if (moved_existing_dest) {
            (void)rename(backup_path, dest_path);
        }
        xSemaphoreTake(charpack->mutex, portMAX_DELAY);
        example_charpack_clear_transfer_locked(charpack);
        xSemaphoreGive(charpack->mutex);
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_INSTALL_FAILED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
        return example_charpack_sink_storage_failed("activate_staged_pack_failed");
    }

    if (example_charpack_write_active_file(charpack, info.pack_id) != ESP_OK) {
        example_charpack_remove_path(dest_path);
        if (moved_existing_dest && rename(backup_path, dest_path) != 0) {
            ESP_LOGW(TAG, "failed to restore previous pack after active-pack write failure");
        }
        xSemaphoreTake(charpack->mutex, portMAX_DELAY);
        example_charpack_clear_transfer_locked(charpack);
        xSemaphoreGive(charpack->mutex);
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_INSTALL_FAILED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
        return example_charpack_sink_storage_failed("set_active_failed");
    }

    if (moved_existing_dest) {
        example_charpack_remove_path(backup_path);
    }

    active_changed = !had_previous_active || !example_charpack_info_matches(&previous_active, &info);
    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    charpack->active_info = info;
    charpack->active_present = true;
    example_charpack_clear_transfer_locked(charpack);
    xSemaphoreGive(charpack->mutex);

    example_charpack_emit_event(charpack,
                                  EXAMPLE_CHARPACK_EVENT_INSTALL_SUCCEEDED,
                                  &info,
                                  NULL,
                                  0,
                                  0,
                                  0);
    if (active_changed) {
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_ACTIVE_CHANGED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
    }
    return example_charpack_sink_ok();
}

static void example_charpack_abort_transfer(void *ctx)
{
    example_charpack_t *charpack = (example_charpack_t *)ctx;
    char staging_path[EXAMPLE_CHARPACK_PATH_MAX];
    example_charpack_info_t info = {0};
    bool emit_abort = false;

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    if (charpack->transfer.active) {
        emit_abort = true;
        example_charpack_safe_copy(info.pack_id, sizeof(info.pack_id), charpack->transfer.pack_id);
        if (snprintf(staging_path,
                     sizeof(staging_path),
                     "%s/%s",
                     charpack->staging_root,
                     charpack->transfer.pack_id) >= (int)sizeof(staging_path)) {
            staging_path[0] = '\0';
        }
        example_charpack_clear_transfer_locked(charpack);
    }
    xSemaphoreGive(charpack->mutex);

    if (staging_path[0] != '\0') {
        example_charpack_remove_path(staging_path);
    }
    if (emit_abort) {
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_TRANSFER_ABORTED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
    }
}

esp_err_t example_charpack_new(const example_charpack_config_t *config,
                                 example_charpack_t **out_charpack)
{
    example_charpack_t *charpack;
    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    esp_err_t ret = ESP_OK;

    if (config == NULL || out_charpack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    charpack = calloc(1, sizeof(*charpack));
    if (charpack == NULL) {
        return ESP_ERR_NO_MEM;
    }

    charpack->mutex = xSemaphoreCreateMutex();
    if (charpack->mutex == NULL) {
        free(charpack);
        return ESP_ERR_NO_MEM;
    }

    charpack->on_event = config->on_event;
    charpack->event_ctx = config->event_ctx;
    charpack->partition_label = CONFIG_EXAMPLE_CHARPACK_SPIFFS_PARTITION_LABEL;
    charpack->format_if_mount_failed = config->format_if_mount_failed;
    charpack->wl_handle = WL_INVALID_HANDLE;
    example_charpack_safe_copy(charpack->mount_point,
                                 sizeof(charpack->mount_point),
                                 config->mount_point != NULL ? config->mount_point
                                                             : CONFIG_EXAMPLE_CHARPACK_MOUNT_POINT);
    example_charpack_safe_copy(charpack->packs_root,
                                 sizeof(charpack->packs_root),
                                 config->packs_root != NULL ? config->packs_root
                                                            : CONFIG_EXAMPLE_CHARPACK_PACKS_ROOT);
    example_charpack_safe_copy(charpack->staging_root,
                                 sizeof(charpack->staging_root),
                                 config->staging_root != NULL ? config->staging_root
                                                              : CONFIG_EXAMPLE_CHARPACK_STAGING_ROOT);
    if (strlcpy(charpack->active_path,
                charpack->mount_point,
                sizeof(charpack->active_path)) >= sizeof(charpack->active_path) ||
        strlcat(charpack->active_path,
                "/",
                sizeof(charpack->active_path)) >= sizeof(charpack->active_path) ||
        strlcat(charpack->active_path,
                EXAMPLE_CHARPACK_ACTIVE_FILENAME,
                sizeof(charpack->active_path)) >= sizeof(charpack->active_path)) {
        vSemaphoreDelete(charpack->mutex);
        free(charpack);
        return ESP_ERR_INVALID_SIZE;
    }

    mount_config.format_if_mount_failed = charpack->format_if_mount_failed;
    mount_config.max_files = 8;
    mount_config.allocation_unit_size = 4096;

    ret = esp_vfs_fat_spiflash_mount_rw_wl(charpack->mount_point,
                                           charpack->partition_label,
                                           &mount_config,
                                           &charpack->wl_handle);
    if (ret != ESP_OK) {
        vSemaphoreDelete(charpack->mutex);
        free(charpack);
        return ret;
    }
    charpack->mounted = true;

    ESP_GOTO_ON_ERROR(example_charpack_ensure_dir(charpack->packs_root), fail, TAG, "packs root");
    ESP_GOTO_ON_ERROR(example_charpack_ensure_dir(charpack->staging_root), fail, TAG, "staging root");

    charpack->sink.begin_transfer = example_charpack_begin_transfer;
    charpack->sink.begin_file = example_charpack_begin_file;
    charpack->sink.write_chunk = example_charpack_write_chunk;
    charpack->sink.end_file = example_charpack_end_file;
    charpack->sink.end_transfer = example_charpack_end_transfer;
    charpack->sink.abort_transfer = example_charpack_abort_transfer;
    charpack->sink.ctx = charpack;

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    example_charpack_refresh_active_from_disk(charpack);
    xSemaphoreGive(charpack->mutex);

    *out_charpack = charpack;
    return ESP_OK;

fail:
    example_charpack_delete(charpack);
    return ret;
}

void example_charpack_delete(example_charpack_t *charpack)
{
    if (charpack == NULL) {
        return;
    }

    example_charpack_abort_transfer(charpack);
    if (charpack->mounted) {
        esp_vfs_fat_spiflash_unmount_rw_wl(charpack->mount_point, charpack->wl_handle);
    }
    if (charpack->mutex != NULL) {
        vSemaphoreDelete(charpack->mutex);
    }
    free(charpack);
}

const esp_desktop_buddy_folder_push_sink_t *example_charpack_get_sink(example_charpack_t *charpack)
{
    if (charpack == NULL) {
        return NULL;
    }
    return &charpack->sink;
}

esp_err_t example_charpack_list(example_charpack_t *charpack,
                                  example_charpack_info_t *items,
                                  size_t capacity,
                                  size_t *out_count)
{
    DIR *dir;
    struct dirent *entry;
    size_t count = 0;

    if (charpack == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dir = opendir(charpack->packs_root);
    if (dir == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    while ((entry = readdir(dir)) != NULL) {
        char pack_root[EXAMPLE_CHARPACK_PATH_MAX];
        example_charpack_info_t info = {0};

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!example_charpack_is_safe_pack_id(entry->d_name)) {
            continue;
        }
        if (snprintf(pack_root,
                     sizeof(pack_root),
                     "%s/%s/manifest.json",
                     charpack->packs_root,
                     entry->d_name) >= (int)sizeof(pack_root)) {
            continue;
        }
        if (example_charpack_read_manifest_info(
                pack_root, entry->d_name, NULL, &info, NULL) != ESP_OK) {
            continue;
        }
        if (items != NULL && count < capacity) {
            items[count] = info;
        }
        count++;
    }

    closedir(dir);
    if (out_count != NULL) {
        *out_count = count;
    }
    return ESP_OK;
}

esp_err_t example_charpack_get_active(example_charpack_t *charpack,
                                        example_charpack_info_t *out_info)
{
    if (charpack == NULL || out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    if (!charpack->active_present) {
        xSemaphoreGive(charpack->mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out_info = charpack->active_info;
    xSemaphoreGive(charpack->mutex);
    return ESP_OK;
}

esp_err_t example_charpack_set_active(example_charpack_t *charpack,
                                        const char *pack_id)
{
    example_charpack_info_t info = {0};
    bool changed = false;
    esp_err_t err;

    if (charpack == NULL || pack_id == NULL || !example_charpack_is_safe_pack_id(pack_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = example_charpack_validate_installed_pack(charpack, pack_id, &info);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    changed = !charpack->active_present || !example_charpack_info_matches(&charpack->active_info, &info);
    xSemaphoreGive(charpack->mutex);
    if (changed) {
        ESP_RETURN_ON_ERROR(example_charpack_write_active_file(charpack, pack_id),
                            TAG,
                            "write active");
    }

    xSemaphoreTake(charpack->mutex, portMAX_DELAY);
    charpack->active_info = info;
    charpack->active_present = true;
    xSemaphoreGive(charpack->mutex);

    if (changed) {
        example_charpack_emit_event(charpack,
                                      EXAMPLE_CHARPACK_EVENT_ACTIVE_CHANGED,
                                      &info,
                                      NULL,
                                      0,
                                      0,
                                      0);
    }
    return ESP_OK;
}
