#include <mcp/texter_ui.h>

#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <lvgl/src/misc/lv_timer_private.h>

typedef struct {
    unsigned bubble_count;
    lv_obj_t * base_obj;
} ctx_t;

static void timer_cb(lv_timer_t * tim)
{
    texter_ui_future_t * fut = lv_timer_get_user_data(tim);
    ctx_t * ctx = texter_ui_future_get_user_data(fut);
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
    texter_ui_future_decref(fut);
}

static void texter_event_cb(texter_ui_t * x, texter_ui_event_type_t type, texter_ui_event_t * e)
{
    texter_ui_convo_t * active_convo = texter_ui_get_active_convo(x);
    texter_ui_future_t * fut = texter_ui_event_get_future(e);
    const char * event_str = NULL;

    switch(type) {
        case TEXTER_UI_EVENT_BUBBLE_LOAD: {
            ctx_t * ctx = texter_ui_get_user_data(x);
            texter_ui_future_incref(fut);
            texter_ui_future_set_user_data(fut, ctx);
            lv_timer_t * timer = lv_timer_create(timer_cb, lv_rand(1000, 2000), fut);
            lv_timer_set_repeat_count(timer, 1);
            lv_timer_set_auto_delete(timer, true);

            event_str = "LOADED";
            break;
        }
        case TEXTER_UI_EVENT_BUBBLE_UNLOAD:
            event_str = "UN-loaded";
            break;
        case TEXTER_UI_EVENT_QUIT: {
            ctx_t * ctx = texter_ui_get_user_data(x);
            lv_obj_delete(ctx->base_obj);
            break;
        }
        default:
            break;
    }

    if(event_str && active_convo && texter_ui_future_get_convo(fut) == active_convo) {
        const char * side_str = texter_ui_event_get_side(e) == TEXTER_UI_SIDE_TOP ? "TOP" : "BOT";
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
            texter_ui_future_decref(fut);
        }

        timer = timer_next;
    }

    ctx_t * ctx = texter_ui_get_user_data(x);
    texter_ui_delete(x);
    free(ctx);
}

void texter_ui_demo_app_run(lv_obj_t * base_obj)
{
    texter_ui_convo_t * convo;

    ctx_t * ctx = calloc(1, sizeof(*ctx));
    ctx->base_obj = base_obj;

    texter_ui_t * x = texter_ui_create(base_obj, texter_event_cb, ctx);
    texter_ui_set_top_text(x, "Texter UI Demo");

    convo = texter_ui_convo_create(x, NULL);
    texter_ui_convo_set_title(convo, "A LoooooooooooOooooooooooooooooOooooooooooooooooooOooooooooooooooooooOoooooooooooooooooong Title");

    for(int i = 1; i < 21; i++) {
        convo = texter_ui_convo_create(x, NULL);

        char title[16];
        snprintf(title, sizeof(title), "Convo #%d", i);
        texter_ui_convo_set_title(convo, title);
    }

    lv_obj_add_event_cb(base_obj, delete_cb, LV_EVENT_DELETE, x);
}

#endif /*CONFIG_MCP_APPS_TEXTER_UI_DEMO*/
