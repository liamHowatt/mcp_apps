#include "beeper_ui_private.h"

static void room_create(void * room_v)
{
    beeper_dict_item_memzero(room_v, sizeof(ui_room_t));
}

// static void room_destroy(void * room_v)
// {
//     ui_room_t * room = room_v;
// }

static void queue_poll_cb(mcp_lvgl_poll_t * handle, int fd, uint32_t revents, void * user_data)
{
    assert(revents == EPOLLIN);

    beeper_ui_t * c = user_data;

    bool popped_one = false;
    while(1) {
        beeper_ui_queue_item_t item;
        assert(0 == pthread_mutex_lock(&c->queue_mutex));
        bool popped = beeper_queue_pop(&c->queue, &item);
        assert(0 == pthread_mutex_unlock(&c->queue_mutex));
        if(!popped) break;
        popped_one = true;

        switch(item.e) {
            case BEEPER_TASK_EVENT_VERIFICATION_STATUS: {
                bool * is_verified_p = item.event_data;
                lv_subject_set_int(&c->verification_status_subject,
                                   *is_verified_p ? BEEPER_UI_VERIFICATION_STATUS_VERIFIED
                                                  : BEEPER_UI_VERIFICATION_STATUS_NOT_VERIFIED);
                free(is_verified_p);
                break;
            }
            case BEEPER_TASK_EVENT_SAS_EMOJI: {
                uint8_t * emoji_ids = item.event_data;
                free((void *) lv_subject_get_pointer(&c->sas_emoji_subject));
                lv_subject_set_pointer(&c->sas_emoji_subject, emoji_ids);
                break;
            }
            case BEEPER_TASK_EVENT_SAS_COMPLETE:
                lv_subject_set_int(&c->verification_status_subject, BEEPER_UI_VERIFICATION_STATUS_VERIFIED);
                break;
            case BEEPER_TASK_EVENT_ROOM_TITLE: {
                ui_room_t * room = beeper_dict_get_create(&c->room_dict, item.event_data, room_create, NULL);
                if(!room->x_convo) {
                    room->x_convo = texter_ui_convo_create(c->x);
                }
                texter_ui_convo_set_title(room->x_convo, item.event_data + (strlen(item.event_data) + 1));
                free(item.event_data);
                break;
            }
            default:
                free(item.event_data);
        }
    }
    assert(popped_one);
}

static void base_obj_delete_cb(lv_event_t * e)
{
    lv_obj_t * base_obj = lv_event_get_target_obj(e);
    beeper_ui_t * c = lv_obj_get_user_data(base_obj);

    if(c->task) beeper_task_destroy(c->task);
    assert(0 == pthread_mutex_destroy(&c->queue_mutex));
    beeper_ui_queue_item_t item;
    while(beeper_queue_pop(&c->queue, &item)) free(item.event_data);
    mcp_lvgl_poll_remove(c->queue_poll_handle);
    beeper_queue_destroy(&c->queue);

    lv_subject_deinit(&c->verification_status_subject);
    free((void *) lv_subject_get_pointer(&c->sas_emoji_subject));
    lv_subject_deinit(&c->sas_emoji_subject);

    beeper_dict_destroy(&c->room_dict, NULL);

    lv_obj_set_user_data(base_obj, NULL);
    free(c);
}

void beeper_ui_base_obj_init(lv_obj_t * base_obj)
{
    beeper_ui_t * c = calloc(1, sizeof(beeper_ui_t));
    assert(c);

    beeper_queue_init(&c->queue, sizeof(beeper_ui_queue_item_t));
    assert(0 == pthread_mutex_init(&c->queue_mutex, NULL));
    c->queue_poll_handle = mcp_lvgl_poll_add(beeper_queue_get_poll_fd(&c->queue), queue_poll_cb, EPOLLIN, c);

    lv_subject_init_int(&c->verification_status_subject, BEEPER_UI_VERIFICATION_STATUS_UNKNOWN);
    lv_subject_init_pointer(&c->sas_emoji_subject, NULL);

    c->x_obj = lv_obj_create(base_obj);
    lv_obj_remove_style_all(c->x_obj);
    lv_obj_add_flag(c->x_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(c->x_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(c->x_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c->x_obj, lv_color_white(), 0);

    c->x = texter_ui_create(c->x_obj);
    texter_ui_set_top_text(c->x, "Beeper");

    beeper_array_init(&c->room_dict, sizeof(ui_room_t));

    lv_obj_set_user_data(base_obj, c);
    lv_obj_add_event_cb(base_obj, base_obj_delete_cb, LV_EVENT_DELETE, NULL);
}

void beeper_ui_task_event_cb(beeper_task_event_t e, void * event_data, void * user_data)
{
    beeper_ui_t * c = user_data;
    beeper_ui_queue_item_t item = {.e = e, .event_data = event_data};
    assert(0 == pthread_mutex_lock(&c->queue_mutex));
    beeper_queue_push(&c->queue, &item);
    assert(0 == pthread_mutex_unlock(&c->queue_mutex));
}
