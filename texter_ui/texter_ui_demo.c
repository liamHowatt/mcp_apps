#include <mcp/texter_ui.h>

#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include <lvgl/src/misc/lv_timer_private.h>

#define CONVO_COUNT 10

typedef struct {
    unsigned bubble_count;
    lv_obj_t * base_obj;
    char * outboxes[CONVO_COUNT];
} ctx_t;

typedef struct {
    ctx_t * ctx;
    texter_ui_convo_t * convo;
    texter_ui_side_t side;
} fut_data_t;

static void timer_cb(lv_timer_t * tim)
{
    texter_ui_future_t * fut = lv_timer_get_user_data(tim);
    fut_data_t * fut_data = texter_ui_future_get_user_data(fut);
    ctx_t * ctx = fut_data->ctx;
    char buf[128];
    switch(ctx->bubble_count % 3) {
        case 0:
            snprintf(buf, sizeof(buf), "msg %u", ctx->bubble_count);
            break;
        case 1:
            snprintf(buf, sizeof(buf), "msg\n%u", ctx->bubble_count);
            break;
        case 2:
            snprintf(buf, sizeof(buf), "Looooooooooo Ooooooooooooooooo Ooooooooooooooooooo Ooooooooooooooooooo Ooooooooooooooooooong msg %u", ctx->bubble_count);
            break;
    }
    texter_ui_future_set_message(fut, buf, ctx->bubble_count % 2);
    ctx->bubble_count += 1;
    free(fut_data);
    texter_ui_future_decref(fut);
}

static void texter_event_cb(texter_ui_t * x, texter_ui_event_type_t type, texter_ui_event_t * e)
{
    ctx_t * ctx = texter_ui_get_user_data(x);
    texter_ui_convo_t * active_convo = texter_ui_get_active_convo(x);
    texter_ui_future_t * fut = texter_ui_event_get_future(e);
    texter_ui_convo_t * convo = texter_ui_event_get_convo(e);
    texter_ui_side_t side = texter_ui_event_get_side(e);
    const char * text = texter_ui_event_get_text(e);
    uintptr_t convo_idx = convo ? (uintptr_t) texter_ui_convo_get_user_data(convo) : 0;
    const char * event_str = NULL;

    switch(type) {
        case TEXTER_UI_EVENT_BUBBLE_LOAD: {
            if(side == TEXTER_UI_SIDE_BOTTOM && ctx->outboxes[convo_idx]) {
                texter_ui_future_set_message(fut, ctx->outboxes[convo_idx], TEXTER_UI_MEMBER_YOU);
                free(ctx->outboxes[convo_idx]);
                ctx->outboxes[convo_idx] = NULL;
                event_str = "LOADED outbox";
            }
            else {
                texter_ui_future_incref(fut);
                fut_data_t * fut_data = malloc(sizeof(*fut_data));
                assert(fut_data);
                fut_data->ctx = ctx;
                fut_data->convo = convo;
                fut_data->side = side;
                texter_ui_future_set_user_data(fut, fut_data);
                lv_timer_t * timer = lv_timer_create(timer_cb, lv_rand(1000, 2000), fut);
                lv_timer_set_repeat_count(timer, 1);
                lv_timer_set_auto_delete(timer, true);
                event_str = "LOADED";
            }

            break;
        }
        case TEXTER_UI_EVENT_BUBBLE_UNLOAD:
            event_str = "UN-loaded";
            break;
        case TEXTER_UI_EVENT_QUIT: {
            puts("QUIT");
            lv_obj_delete(ctx->base_obj);
            break;
        }
        case TEXTER_UI_EVENT_SEND_TEXT: {
            printf("SEND text: %s\n", text);
            lv_timer_t * timer = lv_timer_get_next(NULL);
            while(timer != NULL) {
                lv_timer_t * timer_next = lv_timer_get_next(timer);

                if(timer->timer_cb == timer_cb) {
                    fut = timer->user_data;
                    fut_data_t * fut_data = texter_ui_future_get_user_data(fut);
                    if(fut_data->convo == convo && fut_data->side == TEXTER_UI_SIDE_BOTTOM) {
                        texter_ui_future_set_message(fut, text, TEXTER_UI_MEMBER_YOU);
                        lv_timer_delete(timer);
                        free(fut_data);
                        texter_ui_future_decref(fut);
                        break;
                    }
                }

                timer = timer_next;
            }
            if(!timer) {
                free(ctx->outboxes[convo_idx]);
                ctx->outboxes[convo_idx] = strdup(text);
                assert(ctx->outboxes[convo_idx]);
            }
            break;
        }
        default:
            break;
    }

    if(event_str && active_convo && convo == active_convo) {
        const char * side_str = side == TEXTER_UI_SIDE_TOP ? "TOP" : "BOT";
        printf("%s bubble %s\n", side_str, event_str);
    }
}

static void delete_cb(lv_event_t * e)
{
    texter_ui_t * x = lv_event_get_user_data(e);

    lv_timer_t * timer = lv_timer_get_next(NULL);
    while(timer != NULL) {
        lv_timer_t * timer_next = lv_timer_get_next(timer);

        if(timer->timer_cb == timer_cb) {
            texter_ui_future_t * fut = timer->user_data;
            lv_timer_delete(timer);
            free(texter_ui_future_get_user_data(fut));
            texter_ui_future_decref(fut);
        }

        timer = timer_next;
    }

    ctx_t * ctx = texter_ui_get_user_data(x);
    texter_ui_delete(x);
    for(int i = 0; i < CONVO_COUNT; i++) {
        free(ctx->outboxes[i]);
    }
    free(ctx);
}

void texter_ui_demo_app_run(lv_obj_t * base_obj)
{
    texter_ui_convo_t * convo;

    ctx_t * ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->base_obj = base_obj;

    texter_ui_t * x = texter_ui_create(base_obj, texter_event_cb, ctx);
    texter_ui_set_top_text(x, "Texter UI Demo");

    convo = texter_ui_convo_create(x, (void *)(uintptr_t) 0);
    texter_ui_convo_set_title(convo, "A LoooooooooooOooooooooooooooooOooooooooooooooooooOooooooooooooooooooOoooooooooooooooooong Title");

    convo = texter_ui_convo_create(x, (void *)(uintptr_t) 1);
    texter_ui_convo_set_title(convo, "Convo with sending disabled");
    texter_ui_convo_set_sending_enabled(convo, false);

    for(int i = 2; i < CONVO_COUNT; i++) {
        convo = texter_ui_convo_create(x, (void *)(uintptr_t) i);

        char title[16];
        snprintf(title, sizeof(title), "Convo #%d", i);
        texter_ui_convo_set_title(convo, title);
    }

    lv_obj_add_event_cb(base_obj, delete_cb, LV_EVENT_DELETE, x);
}

#endif /*CONFIG_MCP_APPS_TEXTER_UI_DEMO*/
