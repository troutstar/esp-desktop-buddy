/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shared.h"
#include "example_console.h"

#define BOX_DEMO_CONSOLE_STACK 4096

static void box_demo_charpack_console_print_state(void *ctx, FILE *out)
{
    box_demo_print_state((box_demo_app_t *)ctx, out);
}

static example_console_common_cmds_t s_common_console_cmds = {
    .mutex = NULL,
    .buddy = NULL,
    .transport = NULL,
    .state_cache = NULL,
    .print_state = box_demo_charpack_console_print_state,
    .state_ctx = NULL,
};

static int box_demo_charpack_console_packs(void *ctx, int argc, char **argv)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;
    example_charpack_info_t items[8];
    size_t count = 0;

    (void)argv;

    if (argc != 1) {
        fprintf(stdout, "Usage: packs\n");
        fflush(stdout);
        return 1;
    }

    if (example_charpack_list(app->charpack, items, 8, &count) == ESP_OK) {
        for (size_t i = 0; i < count && i < 8; ++i) {
            fprintf(stdout, "pack[%lu]=%s mode=%d\n",
                    (unsigned long)i,
                    items[i].pack_id,
                    items[i].mode);
        }
    }
    fflush(stdout);
    return 0;
}

static int box_demo_charpack_console_pack(void *ctx, int argc, char **argv)
{
    box_demo_app_t *app = (box_demo_app_t *)ctx;
    example_charpack_info_t items[8];
    char *end = NULL;
    size_t count = 0;
    unsigned long index;

    if (argc == 3 && strcmp(argv[1], "use") == 0) {
        esp_err_t err;

        index = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') {
            fprintf(stdout, "Usage: pack use <index>\n");
            fflush(stdout);
            return 1;
        }

        err = example_charpack_list(app->charpack, items, 8, &count);
        if (err == ESP_OK) {
            if (index >= count || index >= 8) {
                err = ESP_ERR_NOT_FOUND;
            } else {
                err = example_charpack_set_active(app->charpack, items[index].pack_id);
            }
        }

        fprintf(stdout, "pack use rc=%s\n", esp_err_to_name(err));
        fflush(stdout);
        return err == ESP_OK ? 0 : 1;
    }

    fprintf(stdout, "Usage: pack use <index>\n");
    fflush(stdout);
    return 1;
}

static const example_console_command_t s_box_demo_commands[] = {
    {
        .command = "packs",
        .help = "List installed character packs.",
        .hint = NULL,
        .handler = box_demo_charpack_console_packs,
    },
    {
        .command = "pack",
        .help = "Manage the active character pack.",
        .hint = "use <index>",
        .handler = box_demo_charpack_console_pack,
    },
};

static example_console_config_t s_console = {
    .prompt = "box> ",
    .banner =
        "ESP-BOX-3 Desktop Buddy console\n"
        "Type 'help' to list commands.\n",
    .commands = s_box_demo_commands,
    .command_count = sizeof(s_box_demo_commands) / sizeof(s_box_demo_commands[0]),
    .common_cmds = &s_common_console_cmds,
    .ctx = NULL,
    .task_stack_size = BOX_DEMO_CONSOLE_STACK,
    .task_priority = 4,
};

void box_demo_charpack_console_start(box_demo_app_t *app)
{
    s_console.ctx = app;
    s_common_console_cmds.mutex = app->mutex;
    s_common_console_cmds.buddy = app->buddy;
    s_common_console_cmds.transport = app->transport;
    s_common_console_cmds.state_cache = &app->state_cache;
    s_common_console_cmds.state_ctx = app;
    ESP_ERROR_CHECK(example_console_start(&s_console));
}
