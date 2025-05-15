#pragma once

#include "beeper_private.h"

typedef struct beeper_task_t beeper_task_t;

typedef enum {
    BEEPER_TASK_EVENT_VERIFICATION_STATUS,   /* bool: sent once at startup */
    BEEPER_TASK_EVENT_SAS_EMOJI              /* array of 7 uint8_t: the 7 emoji ids, each with range [0-63] */
} beeper_task_event_t;

typedef void (*beeper_task_event_cb_t)(beeper_task_event_t e, void * event_data, void * user_data);

beeper_task_t * beeper_task_create(const char * path, const char * username, const char * password,
                                   beeper_task_event_cb_t event_cb, void * event_cb_user_data);
void beeper_task_destroy(beeper_task_t * t);
