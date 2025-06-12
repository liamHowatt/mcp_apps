#pragma once

#include <lvgl/lvgl.h>

typedef struct texter_ui_t texter_ui_t;
typedef struct texter_ui_convo_t texter_ui_convo_t;

texter_ui_t * texter_ui_create(lv_obj_t * base_obj);
void texter_ui_set_top_text(texter_ui_t * x, const char * text);

texter_ui_convo_t * texter_ui_convo_create(texter_ui_t * x);
void texter_ui_convo_set_title(texter_ui_convo_t * convo, const char * text);

void texter_ui_demo_app_run(lv_obj_t * base_obj);
