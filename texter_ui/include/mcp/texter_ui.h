#pragma once

#include <lvgl/lvgl.h>

typedef struct texter_ui_t texter_ui_t;

texter_ui_t * texter_ui_create(
    lv_obj_t * base_obj,
    const char * top_text
);

void texter_ui_demo_app_run(lv_obj_t * base_obj);
