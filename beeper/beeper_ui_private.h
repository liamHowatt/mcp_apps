#pragma once

#include "beeper_private.h"
#include <lvgl/lvgl.h>
#include "beeper_task.h"
#include <pthread.h>

typedef struct {
    beeper_queue_t queue;
    pthread_mutex_t queue_mutex;
    lv_subject_t verification_status_subject; /* int: beeper_ui_verification_status_t */
    lv_subject_t sas_emoji_subject;           /* pointer: array of 7 uint8_t: the 7 emoji ids, each with range [0-63] */
    beeper_task_t * task;
    lv_timer_t * timer;
} beeper_ui_t;

typedef struct {
    beeper_task_event_t e;
    void * event_data;
} beeper_ui_queue_item_t;

typedef enum {
    BEEPER_UI_VERIFICATION_STATUS_UNKNOWN,
    BEEPER_UI_VERIFICATION_STATUS_VERIFIED,
    BEEPER_UI_VERIFICATION_STATUS_NOT_VERIFIED
} beeper_ui_verification_status_t;

/* beeper_ui_util.c */
lv_obj_t * beeper_ui_base_obj_create(void);
void beeper_ui_task_event_cb(beeper_task_event_t e, void * event_data, void * user_data);

/* beeper_ui_login.c */
void beeper_ui_login(lv_obj_t * base_obj);

/* beeper_ui_verify.c */
void beeper_ui_verify(lv_obj_t * base_obj);

/* beeper_ui_networks.c */
void beeper_ui_networks(lv_obj_t * base_obj);
