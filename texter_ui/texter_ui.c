/*********************
 *      INCLUDES
 *********************/

#include <mcp/texter_ui.h>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/*********************
 *      DEFINES
 *********************/

#define LOAD_DISTANCE_PIXELS_UPPER_THRESH 3000
#define LOAD_DISTANCE_PIXELS_LOWER_THRESH 1000

/**********************
 *      TYPEDEFS
 **********************/

typedef struct ll_t ll_t;
struct ll_t {
    ll_t * up;
    ll_t * down;
};

struct texter_ui_future_t {
    ll_t bubble_link;
    ll_t event_link;
    void * user_data;
    uint32_t refcount;
    texter_ui_convo_t * convo;
    const char * text;
    lv_obj_t * bubble;
    uint8_t member;

    uint8_t event_type;
    uint8_t event_side;

    bool is_latest;
    bool text_is_static;
};
typedef texter_ui_future_t future_t;

struct texter_ui_t {
    lv_obj_t * base_obj;
    texter_ui_event_cb_t event_cb;
    void * user_data;
    ll_t event_list;
    lv_timer_t * event_timer;
    texter_ui_convo_t * active_convo;
    bool * event_cb_stop_ptr;
    bool scroll_is_updating;
};

struct texter_ui_convo_t {
    texter_ui_t * x;
    void * user_data;
    lv_subject_t title_subject;
    lv_obj_t * menuitem;
    ll_t bubble_list;
    bool sending_disabled;
};

struct texter_ui_event_t {
    future_t * fut;
    texter_ui_convo_t * convo;
    texter_ui_side_t side;
    const char * text;
};

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void event_enqueue(texter_ui_t * x, future_t * fut,
                          texter_ui_event_type_t event_type, texter_ui_side_t event_side);
static bool event_cancel(texter_ui_t * x, future_t * fut);
static texter_ui_future_t * future_create(texter_ui_convo_t * convo);
static lv_obj_t * create_bubble(lv_obj_t * parent, const char * message,
                                texter_ui_member_t member);
static void update_bubble(lv_obj_t * bubble, const char * message,
                          texter_ui_member_t member);
static lv_obj_t * create_view(lv_obj_t * parent);
static void convo_back_btn_clicked_cb(lv_event_t * e);
static void convo_send_btn_clicked_cb(lv_event_t * e);
static void convo_menuitem_clicked_cb(lv_event_t * e);
static void event_timer_cb(lv_timer_t * event_timer);
static void scroll_event_cb(lv_event_t * e);
static void update_scroll(texter_ui_t * x);
static void future_set_message(texter_ui_future_t * fut, const char * text,
                               texter_ui_member_t member, bool text_is_static);
static void quit_btn_clicked_cb(lv_event_t * e);

static void list_init(ll_t * list);
static bool list_is_empty(ll_t * list);
static ll_t * list_top(ll_t * list);
static ll_t * list_bottom(ll_t * list);
static void list_add_top(ll_t * list, ll_t * link);
static void list_add_bottom(ll_t * list, ll_t * link);
static void link_init(ll_t * link);
static void link_remove(ll_t * link);
static bool link_is_in_a_list(ll_t * link);
static ll_t * list_link_up(ll_t * list, ll_t * link);
static ll_t * list_link_down(ll_t * list, ll_t * link);

/**********************
 *  STATIC VARIABLES
 **********************/

static const char convo_default_title[] = "(untitled)";

static const lv_style_const_prop_t focus_style_1_props[] = {
    LV_STYLE_CONST_OUTLINE_COLOR(LV_COLOR_MAKE(0x21, 0x96, 0xF3)),
    LV_STYLE_CONST_OUTLINE_WIDTH(2),
    LV_STYLE_CONST_OUTLINE_PAD(1),
    LV_STYLE_CONST_OUTLINE_OPA(LV_OPA_50),
    LV_STYLE_CONST_PROPS_END
};
static LV_STYLE_CONST_INIT(focus_style_1, focus_style_1_props);

static const lv_style_const_prop_t focus_style_2_props[] = {
    LV_STYLE_CONST_BORDER_COLOR(LV_COLOR_MAKE(0x21, 0x96, 0xF3)),
    LV_STYLE_CONST_BORDER_WIDTH(2),
    // LV_STYLE_CONST_BORDER_PAD(0),
    LV_STYLE_CONST_BORDER_OPA(LV_OPA_50),
    LV_STYLE_CONST_PROPS_END
};
static LV_STYLE_CONST_INIT(focus_style_2, focus_style_2_props);

/**********************
 *      MACROS
 **********************/

#define CONTAINER_OF(member_ptr, container_type, member_name) ((container_type *)((uint8_t *)member_ptr - offsetof(container_type, member_name)))

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

texter_ui_t * texter_ui_create(lv_obj_t * base_obj, texter_ui_event_cb_t event_cb, void * user_data)
{
    texter_ui_t * x = calloc(1, sizeof(*x));
    assert(x);

    x->base_obj = base_obj;
    x->event_cb = event_cb;
    x->user_data = user_data;

    list_init(&x->event_list);

    x->event_timer = lv_timer_create(event_timer_cb, 0, x);
    lv_timer_pause(x->event_timer);

    lv_obj_t * view = create_view(base_obj);
    lv_obj_t * top_bar = lv_obj_get_child(view, 0);
    lv_obj_t * top_label = lv_label_create(top_bar);
    lv_label_set_text_static(top_label, "Texter UI");
    lv_obj_t * quit_btn = lv_image_create(top_bar);
    lv_obj_set_align(quit_btn, LV_ALIGN_RIGHT_MID);
    lv_image_set_src(quit_btn, LV_SYMBOL_CLOSE);
    lv_obj_add_flag(quit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quit_btn, quit_btn_clicked_cb, LV_EVENT_CLICKED, x);
    lv_obj_add_style(quit_btn, &focus_style_1, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), quit_btn);
    lv_obj_t * main_area = lv_obj_get_child(view, 1);
    lv_obj_set_flex_flow(main_area, LV_FLEX_FLOW_COLUMN);
    lv_gridnav_add(main_area, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(lv_group_get_default(), main_area);

    lv_group_focus_obj(main_area);

    return x;
}

void * texter_ui_get_user_data(texter_ui_t * x)
{
    return x->user_data;
}

void texter_ui_delete(texter_ui_t * x)
{
    lv_obj_t * convo_menu = lv_obj_get_child(lv_obj_get_child(x->base_obj, 0), 1);

    lv_obj_t * menuitem;
    while((menuitem = lv_obj_get_child(convo_menu, -1))) {
        texter_ui_convo_t * convo = lv_obj_get_user_data(menuitem);
        texter_ui_convo_delete(convo);
    }

    if(x->event_cb_stop_ptr) {
        *x->event_cb_stop_ptr = true;
    }

    assert(list_is_empty(&x->event_list));
    assert(x->active_convo == NULL);

    lv_timer_delete(x->event_timer);
    lv_obj_clean(x->base_obj);

    free(x);
}

void texter_ui_set_top_text(texter_ui_t * x, const char * text)
{
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(x->base_obj, 0), 0), 0), text);
}

texter_ui_convo_t * texter_ui_get_active_convo(texter_ui_t * x)
{
    return x->active_convo;
}

texter_ui_convo_t * texter_ui_convo_create(texter_ui_t * x, void * user_data)
{
    texter_ui_convo_t * convo = calloc(1, sizeof(*convo));
    assert(convo);

    convo->x = x;
    convo->user_data = user_data;

    lv_subject_init_pointer(&convo->title_subject, (void *) convo_default_title);

    convo->menuitem = lv_obj_create(lv_obj_get_child(lv_obj_get_child(x->base_obj, 0), 1));

    list_init(&convo->bubble_list);

    lv_obj_set_user_data(convo->menuitem, convo);
    lv_obj_set_flex_flow(convo->menuitem, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(convo->menuitem, LV_PCT(100), LV_SIZE_CONTENT);
    lv_group_remove_obj(convo->menuitem);
    lv_obj_add_style(convo->menuitem, &focus_style_1, LV_STATE_FOCUS_KEY);

    lv_obj_t * title = lv_label_create(convo->menuitem);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_bind_text(title, &convo->title_subject, NULL);

    lv_obj_t * preview = lv_label_create(convo->menuitem);
    lv_label_set_long_mode(preview, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(preview, LV_PCT(100));
    lv_obj_set_content_height(preview, lv_font_get_line_height(lv_obj_get_style_text_font(preview, 0)));
    lv_label_set_text(preview, "Loading...");

    lv_obj_add_event_cb(convo->menuitem, convo_menuitem_clicked_cb, LV_EVENT_CLICKED, convo);

    future_t * fut = future_create(convo);
    fut->is_latest = true;
    list_add_top(&convo->bubble_list, &fut->bubble_link);
    event_enqueue(x, fut, TEXTER_UI_EVENT_BUBBLE_LOAD, TEXTER_UI_SIDE_TOP);

    return convo;
}

void * texter_ui_convo_get_user_data(texter_ui_convo_t * convo)
{
    return convo->user_data;
}

void texter_ui_convo_delete(texter_ui_convo_t * convo)
{
    texter_ui_t * x = convo->x;

    if(x->active_convo == convo) {
        x->active_convo = NULL;
        lv_obj_delete(lv_obj_get_child(x->base_obj, 1));
        lv_obj_remove_flag(lv_obj_get_child(x->base_obj, 0), LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_delete(convo->menuitem);
    const char * title = lv_subject_get_pointer(&convo->title_subject);
    lv_subject_deinit(&convo->title_subject);
    if(title != convo_default_title) free((char *) title);
    ll_t * link;
    while((link = list_top(&convo->bubble_list))) {
        future_t * fut = CONTAINER_OF(link, future_t, bubble_link);
        link_remove(link);
        event_cancel(x, fut);
        fut->convo = NULL;
        texter_ui_future_decref(fut);
    }
    free(convo);
}

void texter_ui_convo_set_title(texter_ui_convo_t * convo, const char * text)
{
    const char * old_title = lv_subject_get_pointer(&convo->title_subject);
    char * new_title = strdup(text);
    assert(new_title);
    lv_subject_set_pointer(&convo->title_subject, new_title);
    if(old_title != convo_default_title) free((char *) old_title);
}

void texter_ui_convo_set_menu_position(texter_ui_convo_t * convo, int32_t position)
{
    lv_obj_move_to_index(convo->menuitem, position);
}

void texter_ui_convo_set_sending_enabled(texter_ui_convo_t * convo, bool enabled)
{
    convo->sending_disabled = !enabled;

    texter_ui_t * x = convo->x;

    if(convo != x->active_convo) {
        return;
    }

    lv_obj_t * view = lv_obj_get_child(x->base_obj, 1);
    lv_obj_t * compose_bar = lv_obj_get_child(view, 2);
    lv_obj_t * compose_ta = lv_obj_get_child(compose_bar, 0);

    lv_obj_set_state(compose_ta, LV_STATE_DISABLED, !enabled);
}

texter_ui_future_t * texter_ui_event_get_future(texter_ui_event_t * e)
{
    return e->fut;
}

texter_ui_convo_t * texter_ui_event_get_convo(texter_ui_event_t * e)
{
    return e->convo;
}

texter_ui_side_t texter_ui_event_get_side(texter_ui_event_t * e)
{
    return e->side;
}

const char * texter_ui_event_get_text(texter_ui_event_t * e)
{
    return e->text;
}

void texter_ui_future_set_user_data(texter_ui_future_t * fut, void * user_data)
{
    fut->user_data = user_data;
}

void * texter_ui_future_get_user_data(texter_ui_future_t * fut)
{
    return fut->user_data;
}

void texter_ui_future_incref(texter_ui_future_t * fut)
{
    assert(fut->refcount);
    fut->refcount += 1;
    assert(fut->refcount);
}

void texter_ui_future_decref(texter_ui_future_t * fut)
{
    assert(fut->refcount);
    if(--fut->refcount == 0) {
        if(fut->text_is_static == false)
            free((char *) fut->text);
        free(fut);
    }
}

texter_ui_convo_t * texter_ui_future_get_convo(texter_ui_future_t * fut)
{
    return fut->convo;
}

void texter_ui_future_set_message(texter_ui_future_t * fut, const char * text, texter_ui_member_t member)
{
    future_set_message(fut, text, member, false);
}

void texter_ui_future_set_message_static(texter_ui_future_t * fut, const char * text, texter_ui_member_t member)
{
    future_set_message(fut, text, member, true);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void event_enqueue(texter_ui_t * x, future_t * fut,
                          texter_ui_event_type_t event_type, texter_ui_side_t event_side)
{
    fut->event_type = event_type;
    fut->event_side = event_side;
    list_add_bottom(&x->event_list, &fut->event_link);
    lv_timer_resume(x->event_timer);
    texter_ui_future_incref(fut);
}

static bool event_cancel(texter_ui_t * x, future_t * fut)
{
    if(!link_is_in_a_list(&fut->event_link)) {
        return false;
    }

    link_remove(&fut->event_link);
    texter_ui_future_decref(fut);

    if(list_is_empty(&x->event_list)) {
        lv_timer_pause(x->event_timer);
    }

    return true;
}

static texter_ui_future_t * future_create(texter_ui_convo_t * convo)
{
    future_t * fut = calloc(1, sizeof(*fut));
    link_init(&fut->bubble_link);
    link_init(&fut->event_link);
    fut->refcount = 1;
    fut->convo = convo;
    return fut;
}

static lv_obj_t * create_bubble(lv_obj_t * parent, const char * message,
                                texter_ui_member_t member)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    if(message == NULL)
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * bubble = lv_label_create(box);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    if(member == TEXTER_UI_MEMBER_YOU) {
        lv_obj_set_style_bg_color(bubble, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_set_style_text_color(bubble, lv_color_white(), 0);
        lv_obj_set_align(bubble, LV_ALIGN_RIGHT_MID);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_text_color(bubble, lv_color_black(), 0);
        lv_obj_set_align(bubble, LV_ALIGN_LEFT_MID);
    }
    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_width(bubble, LV_PCT(70));
    if(message)
        lv_label_set_text(bubble, message);

    return box;
}

static void update_bubble(lv_obj_t * bubble, const char * message,
                          texter_ui_member_t member)
{
    lv_obj_set_flag(bubble, LV_OBJ_FLAG_HIDDEN, message == NULL);
    lv_obj_t * label = lv_obj_get_child(bubble, 0);
    if(message)
        lv_label_set_text(label, message);
    lv_obj_set_align(label, member == TEXTER_UI_MEMBER_YOU ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID);
}

static lv_obj_t * create_view(lv_obj_t * parent)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);

    /* top bar */
    lv_obj_t * top_bar = lv_obj_create(obj);
    lv_obj_set_size(top_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(top_bar, 8, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);

    /* main area */
    lv_obj_t * main_area = lv_obj_create(obj);
    lv_obj_set_style_bg_opa(main_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(main_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(main_area, 0, 0);
    lv_obj_set_width(main_area, LV_PCT(100));
    lv_obj_set_flex_grow(main_area, 1);

    return obj;
}

static void convo_back_btn_clicked_cb(lv_event_t * e)
{
    texter_ui_convo_t * convo = lv_event_get_user_data(e);
    texter_ui_t * x = convo->x;

    if(x->active_convo != convo) {
        return;
    }

    x->active_convo = NULL;
    lv_obj_delete(lv_obj_get_child(x->base_obj, 1));
    lv_obj_t * convo_menu = lv_obj_get_child(x->base_obj, 0);
    lv_obj_remove_flag(convo_menu, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(lv_obj_get_child(convo_menu, 1));

    ll_t * link;
    while((link = list_top(&convo->bubble_list))) {
        future_t * fut = CONTAINER_OF(link, future_t, bubble_link);

        if(fut->is_latest) {
            fut->bubble = NULL;
            return;
        }

        link_remove(link);

        bool pending_load_was_cancelled = event_cancel(x, fut);

        if(!pending_load_was_cancelled) {
            event_enqueue(x, fut, TEXTER_UI_EVENT_BUBBLE_UNLOAD, TEXTER_UI_SIDE_TOP);
        }

        texter_ui_future_decref(fut);
    }

    future_t * fut = future_create(convo);
    fut->is_latest = true;
    list_add_top(&convo->bubble_list, &fut->bubble_link);
    event_enqueue(x, fut, TEXTER_UI_EVENT_BUBBLE_LOAD, TEXTER_UI_SIDE_TOP);
}

static void convo_send_btn_clicked_cb(lv_event_t * e)
{
    texter_ui_convo_t * convo = lv_event_get_user_data(e);
    texter_ui_t * x = convo->x;

    if(x->active_convo != convo || convo->sending_disabled) {
        return;
    }

    lv_obj_t * view = lv_obj_get_child(x->base_obj, 1);
    lv_obj_t * compose_bar = lv_obj_get_child(view, 2);
    lv_obj_t * compose_ta = lv_obj_get_child(compose_bar, 0);

    const char * text_to_send = lv_textarea_get_text(compose_ta);
    texter_ui_event_t ev = {
        .convo = convo,
        .text = text_to_send,
    };
    x->event_cb(x, TEXTER_UI_EVENT_SEND_TEXT, &ev);

    lv_textarea_set_text(compose_ta, "");
}

static void convo_menuitem_clicked_cb(lv_event_t * e)
{
    texter_ui_convo_t * convo = lv_event_get_user_data(e);
    texter_ui_t * x = convo->x;

    if(x->active_convo) {
        return;
    }

    x->active_convo = convo;

    lv_obj_add_flag(lv_obj_get_child(x->base_obj, 0), LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * view = create_view(x->base_obj);
    lv_obj_t * view_bar = lv_obj_get_child(view, 0);
    lv_obj_t * title = lv_label_create(view_bar);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_align(title, LV_ALIGN_CENTER);
    lv_label_bind_text(title, &convo->title_subject, NULL);
    lv_obj_set_style_max_width(title, LV_PCT(80), 0);
    lv_obj_t * back_btn = lv_image_create(view_bar);
    lv_obj_set_align(back_btn, LV_ALIGN_LEFT_MID);
    lv_image_set_src(back_btn, LV_SYMBOL_LEFT);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, convo_back_btn_clicked_cb, LV_EVENT_CLICKED, convo);
    lv_obj_add_style(back_btn, &focus_style_1, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), back_btn);
    lv_obj_t * bubble_area = lv_obj_get_child(view, 1);
    lv_obj_set_flex_flow(bubble_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(bubble_area, scroll_event_cb, LV_EVENT_SCROLL, x);
    lv_obj_add_style(bubble_area, &focus_style_2, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), bubble_area);
    lv_obj_t * compose_bar = lv_obj_create(view);
    lv_obj_set_flex_flow(compose_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(compose_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(compose_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_t * compose_ta = lv_textarea_create(compose_bar);
    lv_obj_set_flex_grow(compose_ta, 1);
    lv_obj_set_height(compose_ta, 50);
    lv_group_add_obj(lv_group_get_default(), compose_ta);
    if(convo->sending_disabled) lv_obj_add_state(compose_ta, LV_STATE_DISABLED);
    lv_obj_t * send_btn = lv_image_create(compose_bar);
    lv_image_set_src(send_btn, LV_SYMBOL_GPS);
    lv_obj_add_flag(send_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(send_btn, convo_send_btn_clicked_cb, LV_EVENT_CLICKED, convo);
    lv_obj_add_style(send_btn, &focus_style_1, LV_STATE_FOCUS_KEY);
    lv_group_add_obj(lv_group_get_default(), send_btn);

    lv_group_focus_obj(compose_ta);

    ll_t * link = list_top(&convo->bubble_list);

    future_t * fut = CONTAINER_OF(link, future_t, bubble_link);

    if(fut->text) {
        fut->bubble = create_bubble(bubble_area, fut->text, fut->member);
        lv_obj_update_layout(bubble_area);
        int32_t scroll_bottom = lv_obj_get_scroll_bottom(bubble_area);
        lv_obj_scroll_by(bubble_area, 0, -scroll_bottom, LV_ANIM_OFF);

        update_scroll(x);
    }
}

static void event_timer_cb(lv_timer_t * event_timer) {
    texter_ui_t * x = lv_timer_get_user_data(event_timer);

    bool stop;
    x->event_cb_stop_ptr = &stop;

    ll_t * event_link;
    while((event_link = list_top(&x->event_list))) {
        future_t * fut = CONTAINER_OF(event_link, future_t, event_link);

        link_remove(event_link);

        texter_ui_event_t e = {
            .fut = fut,
            .convo = fut->convo,
            .side = fut->event_side,
        };

        x->event_cb(x, fut->event_type, &e);
        texter_ui_future_decref(fut);
        if(stop) return;
    }

    x->event_cb_stop_ptr = NULL;
    lv_timer_pause(event_timer);
}

static void scroll_event_cb(lv_event_t * e)
{
    texter_ui_t * x = lv_event_get_user_data(e);
    update_scroll(x);
}

static void update_scroll(texter_ui_t * x)
{
    if(x->scroll_is_updating) {
        return;
    }
    x->scroll_is_updating = true;

    texter_ui_convo_t * convo = x->active_convo;
    ll_t * link;

    lv_obj_t * bubble_area = lv_obj_get_child(lv_obj_get_child(x->base_obj, 1), 1);
    if(lv_obj_get_child_count(bubble_area) == 0) {
        x->scroll_is_updating = false;
        return;
    }
    lv_obj_update_layout(bubble_area);


    bool new_bottom_added = false;
    link = list_bottom(&convo->bubble_list);
    future_t * fut_bottom = CONTAINER_OF(link, future_t, bubble_link);
    int32_t scroll_bottom = lv_obj_get_scroll_bottom(bubble_area);

    if(fut_bottom->text
       && scroll_bottom < LOAD_DISTANCE_PIXELS_LOWER_THRESH) {
        new_bottom_added = true;
        fut_bottom = future_create(convo);
        list_add_bottom(&convo->bubble_list, &fut_bottom->bubble_link);
        event_enqueue(x, fut_bottom, TEXTER_UI_EVENT_BUBBLE_LOAD, TEXTER_UI_SIDE_BOTTOM);
    }


    bool new_top_added = false;
    link = list_top(&convo->bubble_list);
    future_t * fut_top = CONTAINER_OF(link, future_t, bubble_link);
    int32_t scroll_top = lv_obj_get_scroll_top(bubble_area);

    if(fut_top->text
       && scroll_top < LOAD_DISTANCE_PIXELS_LOWER_THRESH) {
        new_top_added = true;
        fut_top = future_create(convo);
        list_add_top(&convo->bubble_list, &fut_top->bubble_link);
        event_enqueue(x, fut_top, TEXTER_UI_EVENT_BUBBLE_LOAD, TEXTER_UI_SIDE_TOP);
    }


    while(scroll_bottom > LOAD_DISTANCE_PIXELS_UPPER_THRESH
          && (fut_bottom->bubble == NULL || !lv_obj_is_visible(fut_bottom->bubble))) {
        assert(!new_bottom_added);

        if(fut_bottom->bubble) {
            lv_obj_delete(fut_bottom->bubble);
            lv_obj_update_layout(bubble_area);
            scroll_bottom = lv_obj_get_scroll_bottom(bubble_area);
        }

        link_remove(&fut_bottom->bubble_link);
        bool pending_load_was_cancelled = event_cancel(x, fut_bottom);
        if(!pending_load_was_cancelled) {
            event_enqueue(x, fut_bottom, TEXTER_UI_EVENT_BUBBLE_UNLOAD, TEXTER_UI_SIDE_BOTTOM);
        }
        texter_ui_future_decref(fut_bottom);
        link = list_bottom(&convo->bubble_list);
        fut_bottom = CONTAINER_OF(link, future_t, bubble_link);
    }


    while(scroll_top > LOAD_DISTANCE_PIXELS_UPPER_THRESH
          && (fut_top->bubble == NULL || !lv_obj_is_visible(fut_top->bubble))) {
        assert(!new_top_added);

        if(fut_top->bubble) {
            int32_t bottom_before = lv_obj_get_scroll_bottom(bubble_area);
            lv_obj_delete(fut_top->bubble);
            lv_obj_update_layout(bubble_area);
            int32_t bottom_after = lv_obj_get_scroll_bottom(bubble_area);
            lv_obj_scroll_by(bubble_area, 0, bottom_before - bottom_after, LV_ANIM_OFF);

            lv_obj_update_layout(bubble_area);
            scroll_top = lv_obj_get_scroll_top(bubble_area);
        }

        link_remove(&fut_top->bubble_link);
        bool pending_load_was_cancelled = event_cancel(x, fut_top);
        if(!pending_load_was_cancelled) {
            event_enqueue(x, fut_top, TEXTER_UI_EVENT_BUBBLE_UNLOAD, TEXTER_UI_SIDE_TOP);
        }
        texter_ui_future_decref(fut_top);
        link = list_top(&convo->bubble_list);
        fut_top = CONTAINER_OF(link, future_t, bubble_link);
    }


    x->scroll_is_updating = false;
}

static void future_set_message(texter_ui_future_t * fut, const char * text,
                               texter_ui_member_t member, bool text_is_static)
{
    if(!link_is_in_a_list(&fut->bubble_link)
       || (text == NULL && fut->text == NULL)) {
        return;
    }

    texter_ui_convo_t * convo = fut->convo;
    texter_ui_t * x = convo->x;

    if(fut->text_is_static == false) {
        free((char *) fut->text);
    }

    fut->text_is_static = text_is_static;
    if(text_is_static || text == NULL) {
        fut->text = text;
    }
    else {
        fut->text = strdup(text);
        assert(fut->text);
    }
    fut->member = member;

    if(x->active_convo == convo) {
        if(fut->bubble) {
            update_bubble(fut->bubble, text, member);
        }
        else {
            assert(text);

            lv_obj_t * bubble_area = lv_obj_get_child(lv_obj_get_child(x->base_obj, 1), 1);

            ll_t * l_up = list_link_up(&convo->bubble_list, &fut->bubble_link);
            ll_t * l_down = list_link_down(&convo->bubble_list, &fut->bubble_link);

            if(!l_up && !l_down) {
                assert(lv_obj_get_child_count(bubble_area) == 0);
                assert(fut->is_latest);
                fut->bubble = create_bubble(bubble_area, fut->text, fut->member);
                lv_obj_update_layout(bubble_area);
                int32_t scroll_bottom = lv_obj_get_scroll_bottom(bubble_area);
                lv_obj_scroll_by(bubble_area, 0, -scroll_bottom, LV_ANIM_OFF);
            }
            else if(!l_down) {
                lv_obj_update_layout(bubble_area);
                int32_t top_before = lv_obj_get_scroll_top(bubble_area);
                int32_t bottom_before = lv_obj_get_scroll_bottom(bubble_area);

                fut->bubble = create_bubble(bubble_area, text, member);

                if(top_before <= 0 && bottom_before >= 0) {
                    lv_obj_update_layout(bubble_area);
                    int32_t bottom_after = lv_obj_get_scroll_bottom(bubble_area);
                    lv_obj_scroll_by(bubble_area, 0, -bottom_after, LV_ANIM_OFF);
                }

                assert(!fut->is_latest);
                future_t * fut_up = CONTAINER_OF(l_up, future_t, bubble_link);
                if(fut_up->is_latest) {
                    fut_up->is_latest = false;
                    fut->is_latest = true;
                }
            }
            else {
                assert(!l_up);
                assert(!fut->is_latest);

                lv_obj_update_layout(bubble_area);
                int32_t bottom_before = lv_obj_get_scroll_bottom(bubble_area);

                fut->bubble = create_bubble(bubble_area, text, member);
                lv_obj_move_to_index(fut->bubble, 0);

                lv_obj_update_layout(bubble_area);
                int32_t bottom_after = lv_obj_get_scroll_bottom(bubble_area);
                lv_obj_scroll_by(bubble_area, 0, bottom_before - bottom_after, LV_ANIM_OFF);
            }
        }

        update_scroll(x);
    }
    else {
        fut->is_latest = true;

        ll_t * link = list_link_up(&convo->bubble_list, &fut->bubble_link);
        if(link) {
            future_t * fut_up = CONTAINER_OF(link, future_t, bubble_link);
            link_remove(link);
            event_enqueue(x, fut_up, TEXTER_UI_EVENT_BUBBLE_UNLOAD, TEXTER_UI_SIDE_TOP);
            texter_ui_future_decref(fut_up);
        }

        if(list_link_down(&convo->bubble_list, &fut->bubble_link) == NULL) {
            future_t * fut_down = future_create(convo);
            list_add_bottom(&convo->bubble_list, &fut_down->bubble_link);
            event_enqueue(x, fut_down, TEXTER_UI_EVENT_BUBBLE_LOAD, TEXTER_UI_SIDE_BOTTOM);
        }
    }

    if(fut->is_latest) {
        lv_obj_t * preview_label = lv_obj_get_child(convo->menuitem, 1);
        if(member == TEXTER_UI_MEMBER_YOU) {
            if(text) lv_label_set_text_fmt(preview_label, "You: %s", text);
            else lv_label_set_text(preview_label, "You: (deleted)");
        }
        else {
            if(text) lv_label_set_text(preview_label, text);
            else lv_label_set_text(preview_label, "(deleted)");
        }
    }
}

static void quit_btn_clicked_cb(lv_event_t * e)
{
    texter_ui_t * x = lv_event_get_user_data(e);
    texter_ui_event_t ev = {};
    x->event_cb(x, TEXTER_UI_EVENT_QUIT, &ev);
}

static void list_init(ll_t * list)
{
    list->up = list;
    list->down = list;
}

static bool list_is_empty(ll_t * list)
{
    return list->up == list;
}

static ll_t * list_top(ll_t * list)
{
    ll_t * link = list->down;
    if(link == list) return NULL;
    return link;
}

static ll_t * list_bottom(ll_t * list)
{
    ll_t * link = list->up;
    if(link == list) return NULL;
    return link;
}

static void list_add_top(ll_t * list, ll_t * link)
{
    ll_t * old_top = list->down;
    list->down = link;
    link->down = old_top;
    link->up = list;
    old_top->up = link;
}

static void list_add_bottom(ll_t * list, ll_t * link)
{
    ll_t * old_bottom = list->up;
    list->up = link;
    link->up = old_bottom;
    link->down = list;
    old_bottom->down = link;
}

static void link_init(ll_t * link)
{
    link->up = NULL;
    link->down = NULL;
}

static void link_remove(ll_t * link)
{
    link->up->down = link->down;
    link->down->up = link->up;
    link->up = NULL;
    link->down = NULL;
}

static bool link_is_in_a_list(ll_t * link)
{
    return link->up;
}

static ll_t * list_link_up(ll_t * list, ll_t * link)
{
    ll_t * up = link->up;
    if(up == list) return NULL;
    return up;
}

static ll_t * list_link_down(ll_t * list, ll_t * link)
{
    ll_t * down = link->down;
    if(down == list) return NULL;
    return down;
}
