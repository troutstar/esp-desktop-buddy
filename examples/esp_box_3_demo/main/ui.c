/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"
#include "widgets/gif/lv_gif.h"

#include "app_shared.h"
#include "example_app_helpers.h"

#define BOX_DEMO_UI_STACK 8192
#define BOX_DEMO_UI_POLL_MS 200
#define BOX_DEMO_UI_LOCK_TIMEOUT_MS 1000
#define BOX_DEMO_UI_MARGIN 8
#define BOX_DEMO_UI_CARD_WIDTH 304
#define BOX_DEMO_UI_PROMPT_WIDTH 198
#define BOX_DEMO_UI_PROMPT_HEIGHT 126
#define BOX_DEMO_UI_PROMPT_TEXT_WIDTH 182
#define BOX_DEMO_UI_PROMPT_BODY_HEIGHT 58
#define BOX_DEMO_UI_GIF_WIDTH 98
#define BOX_DEMO_UI_GIF_HEIGHT 126
#define BOX_DEMO_UI_GIF_TEXT_WIDTH 78
#define BOX_DEMO_COLOR_BG 0xF3F4F6
#define BOX_DEMO_COLOR_PANEL 0xFFFFFF
#define BOX_DEMO_COLOR_PANEL_ALT 0xE5E7EB
#define BOX_DEMO_COLOR_TEXT 0x111111
#define BOX_DEMO_COLOR_MUTED 0x4B5563
#define BOX_DEMO_COLOR_ALLOW 0x166534
#define BOX_DEMO_COLOR_DENY 0x991B1B
#define BOX_DEMO_GIF_PATH_MAX 224

#define BOX_DEMO_FONT_BODY (&lv_font_montserrat_16)

#if CONFIG_LV_FONT_MONTSERRAT_24
#define BOX_DEMO_FONT_PASSKEY (&lv_font_montserrat_24)
#else
#define BOX_DEMO_FONT_PASSKEY BOX_DEMO_FONT_BODY
#endif

#define BOX_DEMO_FONT_ACTION (&lv_font_montserrat_12)

#define BOX_DEMO_FONT_TITLE (&lv_font_montserrat_16)
#define BOX_DEMO_FONT_META (&lv_font_montserrat_12)

static lv_obj_t *s_title_label;
static lv_obj_t *s_transport_label;
static lv_obj_t *s_sessions_label;
static lv_obj_t *s_prompt_card;
static lv_obj_t *s_prompt_title_label;
static lv_obj_t *s_prompt_body_label;
static lv_obj_t *s_prompt_detail_label;
static lv_obj_t *s_gif_card;
static lv_obj_t *s_gif_obj;
static lv_obj_t *s_gif_label;
static lv_obj_t *s_pack_label;
static lv_obj_t *s_allow_card;
static lv_obj_t *s_allow_label;
static lv_obj_t *s_deny_card;
static lv_obj_t *s_deny_label;
static box_demo_app_t *s_ui_app;
static button_handle_t s_ui_buttons[BSP_BUTTON_NUM];
static char s_gif_pack_id[EXAMPLE_CHARPACK_PACK_ID_MAX + 1];
static char s_gif_src[BOX_DEMO_GIF_PATH_MAX];

static void box_demo_style_label(lv_obj_t *label, const lv_font_t *font, lv_color_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
}

static void box_demo_style_card(lv_obj_t *obj, uint32_t bg_color, uint32_t border_color)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_color), 0);
    lv_obj_set_style_radius(obj, 10, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void box_demo_copy_ellipsized(char *dst,
                                     size_t dst_size,
                                     const char *src,
                                     size_t max_chars)
{
    size_t len;
    size_t keep;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL || src[0] == '\0') {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len <= max_chars || max_chars + 1 >= dst_size) {
        strlcpy(dst, src, dst_size);
        return;
    }

    keep = max_chars > 3 ? max_chars - 3 : max_chars;
    if (keep >= dst_size) {
        keep = dst_size - 1;
    }

    memcpy(dst, src, keep);
    if (keep + 3 < dst_size) {
        memcpy(dst + keep, "...", 3);
        dst[keep + 3] = '\0';
    } else {
        dst[keep] = '\0';
    }
}

static void box_demo_set_action_state(bool prompt_active)
{
    uint32_t allow_bg = prompt_active ? 0xDCFCE7 : BOX_DEMO_COLOR_PANEL_ALT;
    uint32_t deny_bg = prompt_active ? 0xFEE2E2 : BOX_DEMO_COLOR_PANEL_ALT;
    lv_color_t allow_text = lv_color_hex(prompt_active ? BOX_DEMO_COLOR_TEXT : BOX_DEMO_COLOR_MUTED);
    lv_color_t deny_text = lv_color_hex(prompt_active ? BOX_DEMO_COLOR_TEXT : BOX_DEMO_COLOR_MUTED);

    box_demo_style_card(s_allow_card,
                        allow_bg,
                        prompt_active ? BOX_DEMO_COLOR_ALLOW : BOX_DEMO_COLOR_MUTED);
    box_demo_style_card(s_deny_card,
                        deny_bg,
                        prompt_active ? BOX_DEMO_COLOR_DENY : BOX_DEMO_COLOR_MUTED);
    lv_obj_set_style_text_color(s_allow_label, allow_text, 0);
    lv_obj_set_style_text_color(s_deny_label, deny_text, 0);
}

static bool box_demo_path_exists(const char *path)
{
    struct stat st;

    if (path == NULL) {
        return false;
    }

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool box_demo_has_gif_suffix(const char *name)
{
    size_t len;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    if (len < 4) {
        return false;
    }

    return strcasecmp(name + len - 4, ".gif") == 0;
}

static bool box_demo_pick_gif_asset(const char *pack_id,
                                    char *asset_name,
                                    size_t asset_name_size)
{
    static const char *preferred_assets[] = {
        "idle_0.gif",
        "idle.gif",
        "attention.gif",
        "attention_0.gif",
        "busy.gif",
        "sleep.gif",
    };
    char candidate_path[BOX_DEMO_GIF_PATH_MAX];
    char pack_root[BOX_DEMO_GIF_PATH_MAX];
    const char *packs_root = CONFIG_EXAMPLE_CHARPACK_PACKS_ROOT;
    DIR *dir;
    struct dirent *entry;

    if (pack_id == NULL || pack_id[0] == '\0' || asset_name == NULL || asset_name_size == 0) {
        return false;
    }

    if (snprintf(pack_root,
                 sizeof(pack_root),
                 "%s/%s",
                 packs_root,
                 pack_id) >= (int)sizeof(pack_root)) {
        return false;
    }

    for (size_t i = 0; i < sizeof(preferred_assets) / sizeof(preferred_assets[0]); ++i) {
        if (snprintf(candidate_path,
                     sizeof(candidate_path),
                     "%s/%s",
                     pack_root,
                     preferred_assets[i]) >= (int)sizeof(candidate_path)) {
            continue;
        }
        if (box_demo_path_exists(candidate_path)) {
            strlcpy(asset_name, preferred_assets[i], asset_name_size);
            return true;
        }
    }

    dir = opendir(pack_root);
    if (dir == NULL) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!box_demo_has_gif_suffix(entry->d_name)) {
            continue;
        }
        strlcpy(asset_name, entry->d_name, asset_name_size);
        closedir(dir);
        return true;
    }

    closedir(dir);
    return false;
}

static bool box_demo_build_gif_src(const char *pack_id,
                                   char *out_src,
                                   size_t out_src_size)
{
    char asset_name[BOX_DEMO_GIF_PATH_MAX];
    const char *packs_root = CONFIG_EXAMPLE_CHARPACK_PACKS_ROOT;
    const char *mount_point = CONFIG_EXAMPLE_CHARPACK_MOUNT_POINT;
    const char *relative_root = packs_root;
    size_t mount_len;

    if (pack_id == NULL || pack_id[0] == '\0' || out_src == NULL || out_src_size == 0) {
        return false;
    }

    if (!box_demo_pick_gif_asset(pack_id, asset_name, sizeof(asset_name))) {
        return false;
    }

    mount_len = strlen(mount_point);
    if (strncmp(relative_root, mount_point, mount_len) == 0) {
        relative_root += mount_len;
        while (*relative_root == '/') {
            relative_root++;
        }
    }

    return snprintf(out_src,
                    out_src_size,
                    "%c:%s/%s/%s",
                    (char)LV_FS_STDIO_LETTER,
                    relative_root,
                    pack_id,
                    asset_name) < (int)out_src_size;
}

static void box_demo_ui_set_gif_placeholder(const char *text)
{
    lv_gif_set_src(s_gif_obj, NULL);
    lv_obj_add_flag(s_gif_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_gif_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_gif_label, text);
    s_gif_pack_id[0] = '\0';
    s_gif_src[0] = '\0';
}

static void box_demo_ui_update_gif(bool have_active,
                                   const example_charpack_info_t *active_pack)
{
#if CONFIG_LV_USE_GIF
    char desired_src[BOX_DEMO_GIF_PATH_MAX];

    if (!have_active || active_pack == NULL || active_pack->pack_id[0] == '\0') {
        box_demo_ui_set_gif_placeholder("No active pack");
        return;
    }

    if (!box_demo_build_gif_src(active_pack->pack_id, desired_src, sizeof(desired_src))) {
        box_demo_ui_set_gif_placeholder("Pack GIF missing");
        return;
    }

    if (strcmp(s_gif_pack_id, active_pack->pack_id) == 0 &&
        strcmp(s_gif_src, desired_src) == 0) {
        return;
    }

    lv_obj_clear_flag(s_gif_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_gif_label, LV_OBJ_FLAG_HIDDEN);
    lv_gif_set_src(s_gif_obj, desired_src);
    if (!lv_gif_is_loaded(s_gif_obj)) {
        box_demo_ui_set_gif_placeholder("GIF load failed");
        return;
    }

    strlcpy(s_gif_pack_id, active_pack->pack_id, sizeof(s_gif_pack_id));
    strlcpy(s_gif_src, desired_src, sizeof(s_gif_src));
    lv_obj_center(s_gif_obj);
#else
    (void)have_active;
    (void)active_pack;
    box_demo_ui_set_gif_placeholder("GIF support off");
#endif
}

static void box_demo_button_cb(void *button_handle, void *usr_data)
{
    box_demo_app_t *app = (box_demo_app_t *)usr_data;
    button_handle_t button = (button_handle_t)button_handle;
    int button_index = -1;
    esp_desktop_buddy_permission_decision_t decision;

    for (int i = 0; i < BSP_BUTTON_NUM; ++i) {
        if (button == s_ui_buttons[i]) {
            button_index = i;
            break;
        }
    }

    if (app == NULL || app->buddy == NULL) {
        return;
    }

    switch (button_index) {
    case BSP_BUTTON_MAIN:
        decision = ESP_DESKTOP_BUDDY_PERMISSION_DECISION_ONCE;
        break;
    case BSP_BUTTON_CONFIG:
        decision = ESP_DESKTOP_BUDDY_PERMISSION_DECISION_DENY;
        break;
    default:
        ESP_LOGI("box_demo_ui", "button index %d pressed", button_index);
        return;
    }

    if (example_reply_current_prompt(app->mutex,
                                       app->buddy,
                                       app->transport,
                                       &app->state_cache,
                                       decision) != ESP_OK) {
        ESP_LOGW("box_demo_ui", "no active prompt for button index %d", button_index);
    }
}

static void box_demo_copy_or_default(char *dst,
                                     size_t dst_size,
                                     const char *preferred,
                                     const char *fallback)
{
    if (preferred != NULL && preferred[0] != '\0') {
        strlcpy(dst, preferred, dst_size);
    } else if (fallback != NULL) {
        strlcpy(dst, fallback, dst_size);
    } else if (dst_size > 0) {
        dst[0] = '\0';
    }
}

static void box_demo_ui_refresh(box_demo_app_t *app)
{
    example_buddy_state_cache_t state_cache = {0};
    esp_desktop_buddy_transport_ble_state_t transport = {0};
    example_charpack_info_t active_pack = {0};
    char pack_status[BOX_DEMO_STATUS_MAX];
    char display_name[BOX_DEMO_NAME_MAX];
    char advertising_name[BOX_DEMO_BLE_NAME_MAX];
    char advertising_name_compact[BOX_DEMO_BLE_NAME_MAX];
    char owner_name[BOX_DEMO_OWNER_MAX];
    bool have_active;
    bool passkey_active;
    bool prompt_active;
    char title_text[BOX_DEMO_NAME_MAX + BOX_DEMO_OWNER_MAX + 20];
    char transport_text[80];
    char sessions_text[96];
    char pack_text[96];
    char prompt_title[48];
    char prompt_body[BOX_DEMO_STATUS_MAX + EXAMPLE_BUDDY_MESSAGE_MAX + 48];
    char prompt_detail[128];
    char prompt_body_compact[BOX_DEMO_STATUS_MAX + EXAMPLE_BUDDY_MESSAGE_MAX + 48];
    char prompt_detail_compact[128];
    lv_color_t transport_color;

    xSemaphoreTake(app->mutex, portMAX_DELAY);
    state_cache = app->state_cache;
    transport = app->transport_state;
    active_pack = app->active_pack;
    have_active = app->have_active_pack;
    strlcpy(pack_status, app->pack_status, sizeof(pack_status));
    strlcpy(display_name, app->display_name, sizeof(display_name));
    strlcpy(advertising_name, app->advertising_name, sizeof(advertising_name));
    strlcpy(owner_name, app->owner_name, sizeof(owner_name));
    xSemaphoreGive(app->mutex);

    prompt_active = state_cache.has_state && state_cache.prompt.present;
    passkey_active = transport.has_passkey;
    advertising_name_compact[0] = '\0';
    if (advertising_name[0] != '\0') {
        box_demo_copy_ellipsized(advertising_name_compact,
                                 sizeof(advertising_name_compact),
                                 advertising_name,
                                 18);
    }

    if (owner_name[0] != '\0' && display_name[0] != '\0') {
        char owner_compact[BOX_DEMO_OWNER_MAX];
        char display_compact[BOX_DEMO_NAME_MAX];

        box_demo_copy_ellipsized(owner_compact, sizeof(owner_compact), owner_name, 8);
        box_demo_copy_ellipsized(display_compact, sizeof(display_compact), display_name, 10);
        snprintf(title_text, sizeof(title_text), "Hi %s, I am %s!", owner_compact, display_compact);
    } else if (display_name[0] != '\0') {
        char display_compact[BOX_DEMO_NAME_MAX];

        box_demo_copy_ellipsized(display_compact, sizeof(display_compact), display_name, 18);
        snprintf(title_text, sizeof(title_text), "I am %s", display_compact);
    } else if (owner_name[0] != '\0') {
        char owner_compact[BOX_DEMO_OWNER_MAX];

        box_demo_copy_ellipsized(owner_compact, sizeof(owner_compact), owner_name, 14);
        snprintf(title_text, sizeof(title_text), "Hi %s", owner_compact);
    } else {
        strlcpy(title_text, "Desktop Buddy", sizeof(title_text));
    }

    if (passkey_active) {
        if (advertising_name_compact[0] != '\0') {
            snprintf(transport_text,
                     sizeof(transport_text),
                     "Pairing %s",
                     advertising_name_compact);
        } else {
            strlcpy(transport_text, "Bluetooth pairing code", sizeof(transport_text));
        }
        transport_color = lv_color_hex(BOX_DEMO_COLOR_TEXT);
    } else if (!transport.connected) {
        if (advertising_name_compact[0] != '\0') {
            snprintf(transport_text,
                     sizeof(transport_text),
                     "Advertising as %s",
                     advertising_name_compact);
        } else {
            strlcpy(transport_text, "Waiting for Claude over BLE", sizeof(transport_text));
        }
        transport_color = lv_color_hex(BOX_DEMO_COLOR_MUTED);
    } else if (prompt_active) {
        strlcpy(transport_text, "Approval needed now", sizeof(transport_text));
        transport_color = lv_color_hex(BOX_DEMO_COLOR_ALLOW);
    } else if (transport.tx_ready) {
        strlcpy(transport_text, "Connected and ready", sizeof(transport_text));
        transport_color = lv_color_hex(BOX_DEMO_COLOR_ALLOW);
    } else {
        strlcpy(transport_text, "Connected, securing channel", sizeof(transport_text));
        transport_color = lv_color_hex(BOX_DEMO_COLOR_MUTED);
    }

    if (state_cache.has_state) {
        snprintf(sessions_text,
                 sizeof(sessions_text),
                 "Total %lu  Run %lu  Wait %lu",
                 (unsigned long)state_cache.total,
                 (unsigned long)state_cache.running,
                 (unsigned long)state_cache.waiting);
    } else {
        strlcpy(sessions_text, "No session state yet", sizeof(sessions_text));
    }

    snprintf(pack_text,
             sizeof(pack_text),
             have_active ? "Pack: %s" : "Pack: none",
             have_active ? active_pack.pack_id : "");

    if (passkey_active) {
        strlcpy(prompt_title, "Pair with this passkey", sizeof(prompt_title));
        snprintf(prompt_body,
                 sizeof(prompt_body),
                 "%06lu",
                 (unsigned long)transport.passkey);
        strlcpy(prompt_detail,
                "Enter this code in Claude Desktop to finish secure pairing.",
                sizeof(prompt_detail));
        lv_obj_set_style_text_font(s_prompt_body_label, BOX_DEMO_FONT_PASSKEY, 0);
        box_demo_style_card(s_prompt_card, BOX_DEMO_COLOR_PANEL, BOX_DEMO_COLOR_TEXT);
    } else if (prompt_active) {
        snprintf(prompt_title,
                 sizeof(prompt_title),
                 "%s request",
                 state_cache.prompt.tool[0] ? state_cache.prompt.tool : "Approval");
        box_demo_copy_or_default(prompt_body,
                                 sizeof(prompt_body),
                                 state_cache.prompt.hint,
                                 state_cache.msg[0] ? state_cache.msg : "Approve this request?");
        if (state_cache.msg[0] != '\0' &&
            strcmp(state_cache.msg, prompt_body) != 0) {
            strlcpy(prompt_detail, state_cache.msg, sizeof(prompt_detail));
        } else {
            prompt_detail[0] = '\0';
        }
        lv_obj_set_style_text_font(s_prompt_body_label, BOX_DEMO_FONT_BODY, 0);
        box_demo_style_card(s_prompt_card, BOX_DEMO_COLOR_PANEL, BOX_DEMO_COLOR_ALLOW);
    } else {
        strlcpy(prompt_title,
                state_cache.has_state ? "Ready for the next prompt" : "Waiting for connection",
                sizeof(prompt_title));
        box_demo_copy_or_default(prompt_body,
                                 sizeof(prompt_body),
                                 state_cache.has_state ? state_cache.msg : NULL,
                                 "Desktop Buddy shows the next approval prompt here.");
        strlcpy(prompt_detail,
                transport.connected
                    ? "No active prompt. Buttons are idle."
                    : "Pair Claude Desktop to start.",
                sizeof(prompt_detail));
        lv_obj_set_style_text_font(s_prompt_body_label, BOX_DEMO_FONT_BODY, 0);
        box_demo_style_card(s_prompt_card, BOX_DEMO_COLOR_PANEL, BOX_DEMO_COLOR_PANEL_ALT);
    }

    if (!passkey_active) {
        box_demo_copy_ellipsized(prompt_body_compact,
                                 sizeof(prompt_body_compact),
                                 prompt_body,
                                 88);
        box_demo_copy_ellipsized(prompt_detail_compact,
                                 sizeof(prompt_detail_compact),
                                 prompt_detail,
                                 48);
    }

    lv_label_set_text(s_title_label, title_text);
    lv_label_set_text(s_transport_label, transport_text);
    lv_obj_set_style_text_color(s_transport_label, transport_color, 0);
    lv_label_set_text(s_sessions_label, sessions_text);
    lv_label_set_text(s_prompt_title_label, prompt_title);
    lv_label_set_text(s_prompt_body_label, passkey_active ? prompt_body : prompt_body_compact);
    lv_label_set_text(s_prompt_detail_label, passkey_active ? prompt_detail : prompt_detail_compact);
    lv_label_set_text(s_pack_label, pack_text);
    box_demo_ui_update_gif(have_active, &active_pack);
    box_demo_set_action_state(prompt_active);
}

static void box_demo_ui_task(void *arg)
{
    box_demo_app_t *app = (box_demo_app_t *)arg;

    while (true) {
        if (bsp_display_lock(BOX_DEMO_UI_LOCK_TIMEOUT_MS)) {
            box_demo_ui_refresh(app);
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(BOX_DEMO_UI_POLL_MS));
    }
}

esp_err_t box_demo_ui_init(box_demo_app_t *app)
{
    lv_obj_t *scr;

    ESP_RETURN_ON_FALSE(bsp_display_start() != NULL, ESP_FAIL, "box_demo_ui", "display start");
    bsp_display_backlight_on();
    s_ui_app = app;
    memset(s_ui_buttons, 0, sizeof(s_ui_buttons));
    ESP_ERROR_CHECK(bsp_iot_button_create(s_ui_buttons, NULL, BSP_BUTTON_NUM));
    for (int i = 0; i < BSP_BUTTON_NUM; ++i) {
        ESP_ERROR_CHECK(iot_button_register_cb(
            s_ui_buttons[i], BUTTON_PRESS_DOWN, NULL, box_demo_button_cb, s_ui_app));
    }

    if (!bsp_display_lock(BOX_DEMO_UI_LOCK_TIMEOUT_MS)) {
        return ESP_FAIL;
    }

    scr = lv_disp_get_scr_act(NULL);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BOX_DEMO_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(BOX_DEMO_COLOR_TEXT), 0);

    s_title_label = lv_label_create(scr);
    lv_obj_set_pos(s_title_label, BOX_DEMO_UI_MARGIN, 6);
    lv_obj_set_width(s_title_label, BOX_DEMO_UI_CARD_WIDTH);
    box_demo_style_label(s_title_label, BOX_DEMO_FONT_TITLE, lv_color_hex(BOX_DEMO_COLOR_TEXT));
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_title_label, "Desktop Buddy");

    s_transport_label = lv_label_create(scr);
    lv_obj_set_pos(s_transport_label, BOX_DEMO_UI_MARGIN, 26);
    lv_obj_set_width(s_transport_label, BOX_DEMO_UI_CARD_WIDTH);
    box_demo_style_label(s_transport_label, BOX_DEMO_FONT_META, lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_text(s_transport_label, "Waiting for Claude over BLE");

    s_sessions_label = lv_label_create(scr);
    lv_obj_set_pos(s_sessions_label, BOX_DEMO_UI_MARGIN, 42);
    lv_obj_set_width(s_sessions_label, BOX_DEMO_UI_CARD_WIDTH);
    box_demo_style_label(s_sessions_label, BOX_DEMO_FONT_META, lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_text(s_sessions_label, "No session state yet");

    s_prompt_card = lv_obj_create(scr);
    lv_obj_set_pos(s_prompt_card, BOX_DEMO_UI_MARGIN, 58);
    lv_obj_set_size(s_prompt_card, BOX_DEMO_UI_PROMPT_WIDTH, BOX_DEMO_UI_PROMPT_HEIGHT);
    box_demo_style_card(s_prompt_card, BOX_DEMO_COLOR_PANEL, BOX_DEMO_COLOR_PANEL_ALT);

    s_prompt_title_label = lv_label_create(s_prompt_card);
    lv_obj_set_pos(s_prompt_title_label, 8, 8);
    lv_obj_set_width(s_prompt_title_label, BOX_DEMO_UI_PROMPT_TEXT_WIDTH);
    box_demo_style_label(s_prompt_title_label,
                         BOX_DEMO_FONT_META,
                         lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_text(s_prompt_title_label, "Waiting for connection");

    s_prompt_body_label = lv_label_create(s_prompt_card);
    lv_obj_set_pos(s_prompt_body_label, 8, 28);
    lv_obj_set_size(s_prompt_body_label, BOX_DEMO_UI_PROMPT_TEXT_WIDTH, BOX_DEMO_UI_PROMPT_BODY_HEIGHT);
    box_demo_style_label(s_prompt_body_label,
                         BOX_DEMO_FONT_BODY,
                         lv_color_hex(BOX_DEMO_COLOR_TEXT));
    lv_label_set_long_mode(s_prompt_body_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_prompt_body_label, "Desktop Buddy will show the next approval request here.");

    s_prompt_detail_label = lv_label_create(s_prompt_card);
    lv_obj_set_pos(s_prompt_detail_label, 8, 92);
    lv_obj_set_width(s_prompt_detail_label, BOX_DEMO_UI_PROMPT_TEXT_WIDTH);
    box_demo_style_label(s_prompt_detail_label,
                         BOX_DEMO_FONT_META,
                         lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_long_mode(s_prompt_detail_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_prompt_detail_label, "Pair Claude Desktop to start receiving prompts.");

    s_gif_card = lv_obj_create(scr);
    lv_obj_set_pos(s_gif_card, 214, 58);
    lv_obj_set_size(s_gif_card, BOX_DEMO_UI_GIF_WIDTH, BOX_DEMO_UI_GIF_HEIGHT);
    box_demo_style_card(s_gif_card, BOX_DEMO_COLOR_PANEL_ALT, BOX_DEMO_COLOR_PANEL_ALT);

    s_gif_obj = lv_gif_create(s_gif_card);
    lv_gif_set_color_format(s_gif_obj, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_gif_obj);
    lv_obj_add_flag(s_gif_obj, LV_OBJ_FLAG_HIDDEN);

    s_gif_label = lv_label_create(s_gif_card);
    lv_obj_set_width(s_gif_label, BOX_DEMO_UI_GIF_TEXT_WIDTH);
    lv_obj_center(s_gif_label);
    lv_label_set_long_mode(s_gif_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_gif_label, LV_TEXT_ALIGN_CENTER, 0);
    box_demo_style_label(s_gif_label, BOX_DEMO_FONT_META, lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_text(s_gif_label, "No active pack");

    s_pack_label = lv_label_create(scr);
    lv_obj_set_pos(s_pack_label, BOX_DEMO_UI_MARGIN, 188);
    lv_obj_set_width(s_pack_label, BOX_DEMO_UI_CARD_WIDTH);
    box_demo_style_label(s_pack_label, BOX_DEMO_FONT_META, lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_long_mode(s_pack_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_pack_label, "pack <none>");

    s_allow_card = lv_obj_create(scr);
    lv_obj_set_pos(s_allow_card, BOX_DEMO_UI_MARGIN, 204);
    lv_obj_set_size(s_allow_card, 206, 28);
    box_demo_style_card(s_allow_card, BOX_DEMO_COLOR_PANEL_ALT, BOX_DEMO_COLOR_MUTED);

    s_allow_label = lv_label_create(s_allow_card);
    lv_obj_center(s_allow_label);
    box_demo_style_label(s_allow_label,
                         BOX_DEMO_FONT_ACTION,
                         lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_text(s_allow_label, "MAIN ALLOW");

    s_deny_card = lv_obj_create(scr);
    lv_obj_set_pos(s_deny_card, 220, 204);
    lv_obj_set_size(s_deny_card, 92, 28);
    box_demo_style_card(s_deny_card, BOX_DEMO_COLOR_PANEL_ALT, BOX_DEMO_COLOR_MUTED);

    s_deny_label = lv_label_create(s_deny_card);
    lv_obj_center(s_deny_label);
    box_demo_style_label(s_deny_label,
                         BOX_DEMO_FONT_ACTION,
                         lv_color_hex(BOX_DEMO_COLOR_MUTED));
    lv_label_set_text(s_deny_label, "BOOT DENY");

    s_gif_pack_id[0] = '\0';
    s_gif_src[0] = '\0';
    box_demo_set_action_state(false);

    bsp_display_unlock();
    return ESP_OK;
}

void box_demo_ui_start(box_demo_app_t *app)
{
    xTaskCreate(box_demo_ui_task, "box_demo_ui", BOX_DEMO_UI_STACK, app, 4, NULL);
}
