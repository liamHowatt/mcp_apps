#pragma once

#include "beeper_private.h"

typedef struct beeper_task_t beeper_task_t;

typedef enum {
    BEEPER_TASK_DIRECTION_UP,
    BEEPER_TASK_DIRECTION_DOWN
} beeper_task_direction_t;

typedef enum {
    BEEPER_TASK_MEMBER_YOU,
    BEEPER_TASK_MEMBER_THEM
} beeper_task_member_t;

typedef struct {
    char * message_id;
    char * text;
    beeper_task_member_t member;
    uint64_t timestamp;
} beeper_task_event_data_message_t;

typedef struct {
    char * room_id;
    char * this_chunk_id; /* can be NULL */
    char * next_chunk_id; /* can be NULL */
    beeper_task_direction_t direction;
    uint32_t message_count;
    beeper_task_event_data_message_t messages[];
} beeper_task_messages_event_data_t;

typedef struct {
    char * room_id;
    char * message_id;
    char * text;
} beeper_task_message_decrypted_t;

typedef enum {
    BEEPER_TASK_EVENT_VERIFICATION_STATUS,   /* bool: sent once at startup */
    BEEPER_TASK_EVENT_SAS_EMOJI,             /* array of 7 uint8_t: the 7 emoji ids, each with range [0-63] */
    BEEPER_TASK_EVENT_SAS_COMPLETE,          /* NULL */
    BEEPER_TASK_EVENT_ROOM_TITLE,            /* room_id\0title\0 */
    BEEPER_TASK_EVENT_ROOM_MESSAGES,         /* beeper_task_messages_event_data_t */
    BEEPER_TASK_EVENT_MESSAGE_DECRYPTED,     /* beeper_task_message_decrypted_t */
    BEEPER_TASK_EVENT_SENDING_ALLOWED,       /* room_id */
} beeper_task_event_t;

typedef void (*beeper_task_event_cb_t)(beeper_task_event_t e, void * event_data, void * user_data);

beeper_task_t * beeper_task_create(const char * path, const char * username, const char * password,
                                   beeper_task_event_cb_t event_cb, void * event_cb_user_data);
void beeper_task_destroy(beeper_task_t * t);
void beeper_task_event_data_destroy(beeper_task_event_t e, void * event_data);
void beeper_task_sas_matches(beeper_task_t * t);
void beeper_task_request_messages(beeper_task_t * t, const char * room_id, const char * chunk_id,
                                  beeper_task_direction_t direction);
void beeper_task_send_text(beeper_task_t * t, const char * room_id, const char * text);
