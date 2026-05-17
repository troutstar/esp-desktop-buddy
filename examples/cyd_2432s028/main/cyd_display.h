#pragma once

#include <stdint.h>
#include "esp_err.h"

#define CYD_W      320
#define CYD_H      240
#define CYD_STRIP_H 120   /* half-height strip; two flushes per frame */

/* World Y of the strip currently in the framebuffer (0 or CYD_STRIP_H).
   Set this before calling render functions, then call cyd_display_flush(). */
extern int g_cyd_strip_y;

esp_err_t cyd_display_init(void);
uint16_t *cyd_display_fb(void);   /* 320 × CYD_STRIP_H pixels */
void cyd_display_flush(void);     /* blits current strip at g_cyd_strip_y */
