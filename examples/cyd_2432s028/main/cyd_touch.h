#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t cyd_touch_init(void);

/* Returns true if the screen is currently being touched. Sets *x, *y (0-319, 0-239). */
bool cyd_touch_read(int *x, int *y);
