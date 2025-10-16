#pragma once

#include <lvgl/lvgl.h>

typedef struct texter_ui_t texter_ui_t;
typedef struct texter_ui_convo_t texter_ui_convo_t;
typedef struct texter_ui_event_t texter_ui_event_t;
typedef struct texter_ui_future_t texter_ui_future_t;

typedef enum {
    TEXTER_UI_EVENT_BUBBLE_LOAD,
    TEXTER_UI_EVENT_BUBBLE_UNLOAD,
    TEXTER_UI_EVENT_QUIT,
} texter_ui_event_type_t;

typedef enum {
    TEXTER_UI_MEMBER_YOU,
    TEXTER_UI_MEMBER_THEM
} texter_ui_member_t;

typedef enum {
    TEXTER_UI_SIDE_TOP,
    TEXTER_UI_SIDE_BOTTOM
} texter_ui_side_t;

typedef void (*texter_ui_event_cb_t)(texter_ui_t *, texter_ui_event_type_t type, texter_ui_event_t * e);

texter_ui_t * texter_ui_create(lv_obj_t * base_obj, texter_ui_event_cb_t event_cb, void * user_data);
void * texter_ui_get_user_data(texter_ui_t * x);
void texter_ui_delete(texter_ui_t * x);
void texter_ui_set_top_text(texter_ui_t * x, const char * text);
texter_ui_convo_t * texter_ui_get_active_convo(texter_ui_t * x);

texter_ui_convo_t * texter_ui_convo_create(texter_ui_t * x, void * user_data);
void * texter_ui_convo_get_user_data(texter_ui_convo_t * convo);
void texter_ui_convo_delete(texter_ui_convo_t * convo);
void texter_ui_convo_set_title(texter_ui_convo_t * convo, const char * text);
void texter_ui_convo_set_menu_position(texter_ui_convo_t * convo, int32_t position);

texter_ui_future_t * texter_ui_event_get_future(texter_ui_event_t * e);
texter_ui_convo_t * texter_ui_event_get_convo(texter_ui_event_t * e);
texter_ui_side_t texter_ui_event_get_side(texter_ui_event_t * e);

void texter_ui_future_set_user_data(texter_ui_future_t * fut, void * user_data);
void * texter_ui_future_get_user_data(texter_ui_future_t * fut);
void texter_ui_future_incref(texter_ui_future_t * fut);
void texter_ui_future_decref(texter_ui_future_t * fut);
texter_ui_convo_t * texter_ui_future_get_convo(texter_ui_future_t * fut);
void texter_ui_future_set_message(texter_ui_future_t * fut, const char * text, texter_ui_member_t member);
void texter_ui_future_set_message_static(texter_ui_future_t * fut, const char * text, texter_ui_member_t member);

void texter_ui_demo_app_run(lv_obj_t * base_obj);
