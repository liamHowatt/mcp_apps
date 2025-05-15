#include "beeper_ui_private.h"

static void frame_timer_cb(lv_timer_t * t)
{
    beeper_ui_t * c = lv_timer_get_user_data(t);

    while(1) {
        beeper_ui_queue_item_t item;
        assert(0 == pthread_mutex_lock(&c->queue_mutex));
        bool popped = beeper_queue_pop(&c->queue, &item);
        assert(0 == pthread_mutex_unlock(&c->queue_mutex));
        if(!popped) break;

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
            default:
                free(item.event_data);
        }
    }
}

static void base_obj_delete_cb(lv_event_t * e)
{
    lv_obj_t * base_obj = lv_event_get_target_obj(e);
    beeper_ui_t * c = lv_obj_get_user_data(base_obj);

    lv_timer_delete(c->timer);
    if(c->task) beeper_task_destroy(c->task);
    assert(0 == pthread_mutex_destroy(&c->queue_mutex));
    beeper_queue_destroy(&c->queue);

    lv_subject_deinit(&c->verification_status_subject);
    free((void *) lv_subject_get_pointer(&c->sas_emoji_subject));
    lv_subject_deinit(&c->sas_emoji_subject);

    lv_obj_set_user_data(base_obj, NULL);
    free(c);
}

lv_obj_t * beeper_ui_base_obj_create(void)
{
    lv_obj_t * base_obj = lv_obj_create(lv_screen_active());
    beeper_ui_t * c = calloc(1, sizeof(beeper_ui_t));
    assert(c);

    beeper_queue_init(&c->queue, sizeof(beeper_ui_queue_item_t));
    assert(0 == pthread_mutex_init(&c->queue_mutex, NULL));

    lv_subject_init_int(&c->verification_status_subject, BEEPER_UI_VERIFICATION_STATUS_UNKNOWN);
    lv_subject_init_pointer(&c->sas_emoji_subject, NULL);

    lv_obj_set_user_data(base_obj, c);
    lv_obj_add_event_cb(base_obj, base_obj_delete_cb, LV_EVENT_DELETE, NULL);

    c->timer = lv_timer_create(frame_timer_cb, LV_DEF_REFR_PERIOD, c);

    return base_obj;
}

void beeper_ui_task_event_cb(beeper_task_event_t e, void * event_data, void * user_data)
{
    beeper_ui_t * c = user_data;
    beeper_ui_queue_item_t item = {.e = e, .event_data = event_data};
    assert(0 == pthread_mutex_lock(&c->queue_mutex));
    beeper_queue_push(&c->queue, &item);
    assert(0 == pthread_mutex_unlock(&c->queue_mutex));
}
