#pragma once

#include "esp_err.h"

typedef struct cyd_app_s cyd_app_t;

esp_err_t cyd_ui_init(cyd_app_t *app);
