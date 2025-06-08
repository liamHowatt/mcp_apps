#include "beeper_ui_private.h"

static void queue_poll_cb(int fd, uint32_t revents, void * user_data)
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
