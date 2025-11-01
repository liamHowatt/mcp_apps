#include "beeper_ui_private.h"

static ui_message_t * message_create(void)
{
    ui_message_t * msg = beeper_asserting_calloc(1, sizeof(*msg));
    beeper_ll_link_init(&msg->link);
    return msg;
}

static void message_destroy(ui_message_t * msg)
{
    beeper_rcstr_decref(msg->next_chunk_id);
    free(msg->message_id);
    free(msg->text);
    free(msg);
}

static ui_room_t * room_create(void)
{
    ui_room_t * room = beeper_asserting_calloc(1, sizeof(*room));
    beeper_ll_link_init(&room->link);
    beeper_ll_list_init(&room->msg_list);
    return room;
}

static void room_destroy(ui_room_t * room)
{
    ui_message_t * msg;
    while((msg = (ui_message_t *) beeper_ll_list_top(&room->msg_list))) {
        beeper_ll_link_remove(&msg->link);
        message_destroy(msg);
    }
    texter_ui_convo_delete(room->x_convo);
    free(room->room_id);
    beeper_rcstr_decref(room->bubble_top_requested_chunk_id);
    beeper_rcstr_decref(room->bubble_bottom_requested_chunk_id);
    free(room);
}

static ui_room_t * room_get_create(beeper_ui_t * c, const char * room_id)
{
    ui_room_t * room = NULL;
    while((room = (ui_room_t *) beeper_ll_list_link_down(&c->room_list, (beeper_ll_t *) room))) {
        if(0 == strcmp(room_id, room->room_id)) break;
    }
    if(room == NULL) {
        room = room_create();
        beeper_ll_list_add_bottom(&c->room_list, (beeper_ll_t *) room);
        room->room_id = beeper_asserting_strdup(room_id);
        room->x_convo = texter_ui_convo_create(c->x, room);
        texter_ui_convo_set_sending_enabled(room->x_convo, false);
    }
    return room;
}

static int msgcmp(uint64_t left_timestamp, const char * left_message_id,
                  uint64_t right_timestamp, const char * right_message_id)
{
    if(left_timestamp < right_timestamp) return -1;
    if(left_timestamp > right_timestamp) return 1;
    return strcmp(left_message_id, right_message_id);
}

static void move_to_ui_message_from_event_message(ui_message_t * msg, beeper_task_event_data_message_t * mm,
                                                  beeper_rcstr_t * incremented_next_chunk_id_rcstr)
{
    msg->next_chunk_id = incremented_next_chunk_id_rcstr;
    msg->message_id = mm->message_id; /* take ownership */
    mm->message_id = NULL;
    msg->text = mm->text; /* take ownership */
    mm->text = NULL;
    msg->member = mm->member == BEEPER_TASK_MEMBER_YOU ? TEXTER_UI_MEMBER_YOU : TEXTER_UI_MEMBER_THEM;
    msg->timestamp = mm->timestamp;
}

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
                bool is_verified = item.event_data;
                lv_subject_set_int(&c->verification_status_subject,
                                   is_verified ? BEEPER_UI_VERIFICATION_STATUS_VERIFIED
                                               : BEEPER_UI_VERIFICATION_STATUS_NOT_VERIFIED);
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
                char * room_id = item.event_data;
                char * title = room_id + (strlen(room_id) + 1);
                ui_room_t * room = room_get_create(c, room_id);
                texter_ui_convo_set_title(room->x_convo, title);
                free(item.event_data);
                break;
            }
            case BEEPER_TASK_EVENT_ROOM_MESSAGES: {
                beeper_task_messages_event_data_t * m = item.event_data;
                ui_room_t * room = room_get_create(c, m->room_id);

                /* decide whether to incorporate this chunk */
                bool process = false;

                if(m->this_chunk_id) {
                    /* this is a request we made at some point. do we still want it? */

                    beeper_rcstr_t ** request_chunk_id_p = NULL;

                    if(m->direction == BEEPER_TASK_DIRECTION_UP) {
                        request_chunk_id_p = &room->bubble_top_requested_chunk_id;
                    }
                    else { /* m->direction == BEEPER_TASK_DIRECTION_DOWN */
                        request_chunk_id_p = &room->bubble_bottom_requested_chunk_id;
                    }

                    if(*request_chunk_id_p && 0 == strcmp(m->this_chunk_id, beeper_rcstr_str(*request_chunk_id_p))) {
                        beeper_rcstr_decref(*request_chunk_id_p);
                        *request_chunk_id_p = NULL;
                        process = true;
                    }
                }
#if BEEPER_NEW_MESSAGE_READ_AHEAD > 0
                else { /* m->this_chunk_id == NULL */
                    /* are we keeping new messages? 1. are we scrolled to the bottom and 2. do we not have too many already */

                    if(room->msg_window_is_not_caught_up == false) {
                        ui_message_t * msg = room->bubble_bottom;
                        if(msg) {
                            /* loop through and make sure there aren't over `BEEPER_NEW_MESSAGE_READ_AHEAD` messages
                               the bubble isn't caught up with */
                            uint32_t unrendered_message_count = 0;
                            process = true;
                            while((msg = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link))) {
                                unrendered_message_count += 1;
                                if(unrendered_message_count >= BEEPER_NEW_MESSAGE_READ_AHEAD) {
                                    process = false;
                                    room->msg_window_is_not_caught_up = true;
                                    break;
                                }
                            }
                        }
                    }
                }
#endif

                if(!process) {
                    beeper_task_event_data_destroy(item.e, item.event_data);
                    break;
                }

                /*
                m->this_chunk_id            m->next_chunk_id            this is a chunk we requested and there's more
                m->this_chunk_id            m->next_chunk_id == NULL    this is a chunk we requested and it was the last one
                m->this_chunk_id == NULL    m->next_chunk_id            this is a new messages chunk and it has a prev chunk id
                m->this_chunk_id == NULL    m->next_chunk_id == NULL    this is a new messages chunk and there are no earlier events
                */
                char * new_next_chunk_id;
                beeper_rcstr_t ** new_next_chunk_id_rcstr_p;
                beeper_rcstr_t * next_chunk_id_rcstr = NULL;
                beeper_rcstr_t * this_chunk_id_rcstr = NULL;
                if(m->next_chunk_id) {
                    new_next_chunk_id = m->next_chunk_id;
                    new_next_chunk_id_rcstr_p = &next_chunk_id_rcstr;
                }
                else {
                    new_next_chunk_id = m->this_chunk_id;
                    new_next_chunk_id_rcstr_p = &this_chunk_id_rcstr;
                }

                /* update the messages whose IDs we already have */
                for(uint32_t i = 0; i < m->message_count; i++) {
                    beeper_task_event_data_message_t * mm = &m->messages[i];

                    ui_message_t * msg = NULL;
                    while((msg = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link))) {
                        if(msg->message_id && 0 == strcmp(mm->message_id, msg->message_id)) break;
                    }
                    if(msg == NULL) continue;

                    msg->member = mm->member == BEEPER_TASK_MEMBER_YOU ? TEXTER_UI_MEMBER_YOU : TEXTER_UI_MEMBER_THEM;
                    if(msg->fut) {
                        if(mm->text == NULL && msg->text) {
                            mm->text = beeper_asserting_strdup("(deleted)");
                        }
                        if(mm->text && (msg->text == NULL || 0 != strcmp(mm->text, msg->text))) {
                            texter_ui_future_set_message_static(msg->fut, mm->text, msg->member);
                            free(msg->text);
                            msg->text = mm->text; /* take ownership */
                            mm->text = NULL;
                        }
                    }
                    free(mm->message_id);
                    mm->message_id = NULL; /* ignore this message in the next block */
                }

                /* incorporate new messages */
                for(uint32_t i = 0; i < m->message_count; i++) {
                    beeper_task_event_data_message_t * mm = &m->messages[i];
                    if(mm->message_id == NULL) continue;

                    if(m->direction == BEEPER_TASK_DIRECTION_UP) {
                        /* scan til we find a timestamp greater than ours */
                        ui_message_t * msg = NULL;
                        while((msg = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link))) {
                            if(msg->message_id
                               && msgcmp(mm->timestamp, mm->message_id, msg->timestamp, msg->message_id)
                                  < 0) break;
                        }

                        /* if the message above is a waiting future, use it */
                        ui_message_t * msg_up = (ui_message_t *) beeper_ll_list_link_up(&room->msg_list, &msg->link);
                        if(msg_up && msg_up->message_id == NULL) msg = msg_up;

                        if(msg == NULL || msg->message_id) {
                            /* this is not a waiting future. insert a new msg above it */

                            ui_message_t * new_msg = message_create();
                            beeper_ll_list_link_insert_above(&room->msg_list, &new_msg->link, &msg->link);
                            move_to_ui_message_from_event_message(new_msg, mm, beeper_rcstr_create_maybe(new_next_chunk_id_rcstr_p, new_next_chunk_id));

                            /* shift over all the futures above to close the hole */
                            msg = new_msg;
                            while((msg_up = (ui_message_t *) beeper_ll_list_link_up(&room->msg_list, &msg->link)) && msg_up->fut) {
                                msg->fut = msg_up->fut;
                                msg_up->fut = NULL;
                                texter_ui_future_set_message_static(msg->fut, msg->text, msg->member); /* NULL text ok */

                                if(room->bubble_top == msg_up) {
                                    room->bubble_top = msg;
                                }
                                if(room->bubble_bottom == msg_up) {
                                    room->bubble_bottom = msg;
                                }

                                msg = msg_up;
                            }
                            msg = (ui_message_t *) beeper_ll_list_top(&room->msg_list);
                            if(msg->fut == NULL && msg->message_id == NULL) {
                                beeper_ll_link_remove(&msg->link);
                                message_destroy(msg);
                            }
                        }
                        else {
                            /* this is a waiting future. set its text, if there is text */

                            move_to_ui_message_from_event_message(msg, mm, beeper_rcstr_create_maybe(new_next_chunk_id_rcstr_p, new_next_chunk_id));
                            if(msg->text) texter_ui_future_set_message_static(msg->fut, msg->text, msg->member);
                        }
                    }
                    else { /* m->direction == BEEPER_TASK_DIRECTION_DOWN */
                        /* scan til we find a timestamp less than ours */
                        ui_message_t * msg = NULL;
                        while((msg = (ui_message_t *) beeper_ll_list_link_up(&room->msg_list, &msg->link))) {
                            if(msg->message_id
                               && msgcmp(msg->timestamp, msg->message_id, mm->timestamp, mm->message_id)
                                  < 0) break;
                        }

                        /* if the message below is a waiting future, use it */
                        ui_message_t * msg_down = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link);
                        if(msg_down && msg_down->message_id == NULL) msg = msg_down;

                        if(msg == NULL || msg->message_id) {
                            /* this is not a waiting future. insert a new msg below it */

                            ui_message_t * new_msg = message_create();
                            beeper_ll_list_link_insert_below(&room->msg_list, &new_msg->link, &msg->link);
                            move_to_ui_message_from_event_message(new_msg, mm, beeper_rcstr_create_maybe(new_next_chunk_id_rcstr_p, new_next_chunk_id));

                            /* shift over all the futures above to close the hole */
                            msg = new_msg;
                            while((msg_down = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link)) && msg_down->fut) {
                                msg->fut = msg_down->fut;
                                msg_down->fut = NULL;
                                texter_ui_future_set_message_static(msg->fut, msg->text, msg->member); /* NULL text ok */

                                if(room->bubble_bottom == msg_down) {
                                    room->bubble_bottom = msg;
                                }
                                if(room->bubble_top == msg_down) {
                                    room->bubble_top = msg;
                                }

                                msg = msg_down;
                            }
                            msg = (ui_message_t *) beeper_ll_list_bottom(&room->msg_list);
                            if(msg->fut == NULL && msg->message_id == NULL) {
                                beeper_ll_link_remove(&msg->link);
                                message_destroy(msg);
                            }
                        }
                        else {
                            /* this is a waiting future. set its text, if there is text */

                            move_to_ui_message_from_event_message(msg, mm, beeper_rcstr_create_maybe(new_next_chunk_id_rcstr_p, new_next_chunk_id));
                            if(msg->text) texter_ui_future_set_message_static(msg->fut, msg->text, msg->member);
                        }
                    }
                }

                /* send a request for more messages if needed */
                if(m->direction == BEEPER_TASK_DIRECTION_UP) {
                    if(m->this_chunk_id && m->next_chunk_id
                       && room->bubble_top && room->bubble_top->message_id == NULL) {
                        room->bubble_top_requested_chunk_id = beeper_rcstr_create_maybe(&next_chunk_id_rcstr, m->next_chunk_id);
                        beeper_task_request_messages(c->task, room->room_id,
                                                     beeper_rcstr_str(room->bubble_top_requested_chunk_id),
                                                     BEEPER_TASK_DIRECTION_UP);
                    }
                }
                else { /* m->direction == BEEPER_TASK_DIRECTION_DOWN */
                    if(m->this_chunk_id) {
                        /* it's a request we made */
                        if(m->next_chunk_id) {
                            /* there's a next chunk to get */
                            if(room->bubble_bottom && room->bubble_bottom->message_id == NULL) {
                                room->bubble_bottom_requested_chunk_id = beeper_rcstr_create_maybe(&next_chunk_id_rcstr,
                                                                                                   m->next_chunk_id);
                                beeper_task_request_messages(c->task, room->room_id,
                                                             beeper_rcstr_str(room->bubble_bottom_requested_chunk_id),
                                                             BEEPER_TASK_DIRECTION_DOWN);
                            }
                        }
                        else { /* m->next_chunk_id == NULL */
                            /* this was the last chunk. there are no more */
                            if(room->msg_window_is_not_caught_up) {
                                /* we just became caught up */
                                room->msg_window_is_not_caught_up = false;
                                /* send one final sporadic request to deal with a theoretical new message race condition */
                                /* use this_chunk_id since it's the only chunk id available to work with */
                                room->bubble_bottom_requested_chunk_id = beeper_rcstr_create_maybe(&this_chunk_id_rcstr,
                                                                                                   m->this_chunk_id);
                                beeper_task_request_messages(c->task, room->room_id,
                                                             beeper_rcstr_str(room->bubble_bottom_requested_chunk_id),
                                                             BEEPER_TASK_DIRECTION_DOWN);
                            }
                        }
                    }
                }

                beeper_task_event_data_destroy(item.e, item.event_data);
                break;
            }
            case BEEPER_TASK_EVENT_MESSAGE_DECRYPTED: {
                beeper_task_message_decrypted_t * d = item.event_data;
                ui_room_t * room = room_get_create(c, d->room_id);
                ui_message_t * msg = NULL;
                while((msg = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link))) {
                    if(msg->message_id && 0 == strcmp(d->message_id, msg->message_id)) break;
                }
                if(msg == NULL || (msg->text && 0 == strcmp(d->text, msg->text))) {
                    beeper_task_event_data_destroy(item.e, item.event_data);
                    break;
                }

                if(msg->fut) texter_ui_future_set_message_static(msg->fut, d->text, msg->member);
                free(msg->text);
                msg->text = d->text; /* take ownership */
                d->text = NULL;

                beeper_task_event_data_destroy(item.e, item.event_data);
                break;
            }
            case BEEPER_TASK_EVENT_SENDING_ALLOWED:
                char * room_id = item.event_data;
                ui_room_t * room = room_get_create(c, room_id);
                texter_ui_convo_set_sending_enabled(room->x_convo, true);
                free(item.event_data);
                break;
            default:
                beeper_task_event_data_destroy(item.e, item.event_data);
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

    ui_room_t * room;
    while((room = (ui_room_t *) beeper_ll_list_top(&c->room_list))) {
        beeper_ll_link_remove(&room->link);
        room_destroy(room);
    }

    texter_ui_delete(c->x);

    lv_obj_set_user_data(base_obj, NULL);
    free(c);
}

static void texter_event_cb(texter_ui_t * x, texter_ui_event_type_t type, texter_ui_event_t * e)
{
    beeper_ui_t * c = texter_ui_get_user_data(x);

    switch(type) {
        case TEXTER_UI_EVENT_BUBBLE_LOAD:
        case TEXTER_UI_EVENT_BUBBLE_UNLOAD:
        case TEXTER_UI_EVENT_SEND_TEXT:
            break;
        case TEXTER_UI_EVENT_QUIT:
            lv_obj_delete(lv_obj_get_parent(c->x_obj));
            return;
        default:
            return;
    }

    texter_ui_convo_t * x_convo = texter_ui_event_get_convo(e);
    ui_room_t * room = texter_ui_convo_get_user_data(x_convo);
    texter_ui_side_t side = texter_ui_event_get_side(e);
    texter_ui_future_t * fut = texter_ui_event_get_future(e);
    const char * text = texter_ui_event_get_text(e);

    switch(type) {
        case TEXTER_UI_EVENT_BUBBLE_LOAD:
            if(side == TEXTER_UI_SIDE_TOP) {
                beeper_rcstr_t * next_chunk_id = NULL;
                ui_message_t * msg = room->bubble_top;
                if(msg) {
                    next_chunk_id = msg->next_chunk_id;
                    msg = (ui_message_t *) beeper_ll_list_link_up(&room->msg_list, &msg->link);
                }
                else {
                    msg = (ui_message_t *) beeper_ll_list_bottom(&room->msg_list); /* not a typo */
                }
                if(msg == NULL) {
                    msg = message_create();
                    beeper_ll_list_add_top(&room->msg_list, &msg->link);
                }
                msg->fut = fut;
                if(msg->text) {
                    texter_ui_future_set_message_static(fut, msg->text, msg->member);
                }
                else if(msg->message_id == NULL) {
                    if(room->bubble_top_requested_chunk_id == NULL) {
                        room->bubble_top_requested_chunk_id = next_chunk_id;
                        beeper_rcstr_incref(next_chunk_id);
                        beeper_task_request_messages(c->task, room->room_id, beeper_rcstr_str(next_chunk_id), BEEPER_TASK_DIRECTION_UP);
                    }
                }
                room->bubble_top = msg;
                if(room->bubble_bottom == NULL) room->bubble_bottom = msg;
            }
            else { /* side == TEXTER_UI_SIDE_BOTTOM */
                beeper_rcstr_t * next_chunk_id = NULL;
                ui_message_t * msg = room->bubble_bottom;
                if(msg) {
                    next_chunk_id = msg->next_chunk_id;
                    msg = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link);
                }
                else {
                    msg = (ui_message_t *) beeper_ll_list_bottom(&room->msg_list);
                }
                if(msg == NULL) {
                    msg = message_create();
                    beeper_ll_list_add_bottom(&room->msg_list, &msg->link);
                }
                msg->fut = fut;
                if(msg->text) {
                    texter_ui_future_set_message_static(fut, msg->text, msg->member);
                }
                else if(msg->message_id == NULL) {
                    if(room->msg_window_is_not_caught_up && room->bubble_bottom_requested_chunk_id == NULL) {
                        room->bubble_bottom_requested_chunk_id = next_chunk_id;
                        beeper_rcstr_incref(next_chunk_id);
                        beeper_task_request_messages(c->task, room->room_id, beeper_rcstr_str(next_chunk_id), BEEPER_TASK_DIRECTION_DOWN);
                    }
                }
                room->bubble_bottom = msg;
                if(room->bubble_top == NULL) room->bubble_top = msg;
            }
            break;
        case TEXTER_UI_EVENT_BUBBLE_UNLOAD:
            if(side == TEXTER_UI_SIDE_TOP) {
                beeper_rcstr_decref(room->bubble_top_requested_chunk_id);
                room->bubble_top_requested_chunk_id = NULL;
                ui_message_t * msg = room->bubble_top;
                if(msg == room->bubble_bottom) {
                    room->msg_window_is_not_caught_up = false;
                    room->bubble_top = NULL;
                    room->bubble_bottom = NULL;
                    while((msg = (ui_message_t *) beeper_ll_list_top(&room->msg_list))) {
                        beeper_ll_link_remove(&msg->link);
                        message_destroy(msg);
                    }
                    beeper_rcstr_decref(room->bubble_bottom_requested_chunk_id);
                    room->bubble_bottom_requested_chunk_id = NULL;
                } else {
                    room->bubble_top = (ui_message_t *) beeper_ll_list_link_down(&room->msg_list, &msg->link);
                    while(room->bubble_top != (msg = (ui_message_t *) beeper_ll_list_top(&room->msg_list))) {
                        beeper_ll_link_remove(&msg->link);
                        message_destroy(msg);
                    }
                }
            }
            else { /* side == TEXTER_UI_SIDE_BOTTOM */
                beeper_rcstr_decref(room->bubble_bottom_requested_chunk_id);
                room->bubble_bottom_requested_chunk_id = NULL;
                ui_message_t * msg = room->bubble_bottom;
                if(msg == room->bubble_top) {
                    room->msg_window_is_not_caught_up = false;
                    room->bubble_top = NULL;
                    room->bubble_bottom = NULL;
                    while((msg = (ui_message_t *) beeper_ll_list_top(&room->msg_list))) {
                        beeper_ll_link_remove(&msg->link);
                        message_destroy(msg);
                    }
                    beeper_rcstr_decref(room->bubble_top_requested_chunk_id);
                    room->bubble_top_requested_chunk_id = NULL;
                } else {
                    room->msg_window_is_not_caught_up = true;
                    room->bubble_bottom = (ui_message_t *) beeper_ll_list_link_up(&room->msg_list, &msg->link);
                    while(room->bubble_bottom != (msg = (ui_message_t *) beeper_ll_list_bottom(&room->msg_list))) {
                        beeper_ll_link_remove(&msg->link);
                        message_destroy(msg);
                    }
                }
            }
            break;
        case TEXTER_UI_EVENT_SEND_TEXT:
            beeper_task_send_text(c->task, room->room_id, text);
            break;
        default:
            break;
    }
}

void beeper_ui_base_obj_init(lv_obj_t * base_obj)
{
    beeper_ui_t * c = beeper_asserting_calloc(1, sizeof(beeper_ui_t));

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

    c->x = texter_ui_create(c->x_obj, texter_event_cb, c);
    texter_ui_set_top_text(c->x, "Beeper");

    beeper_ll_list_init(&c->room_list);

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
