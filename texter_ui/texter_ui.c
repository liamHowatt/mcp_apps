#include <mcp/texter_ui.h>

#include <assert.h>
#include <stdlib.h>

struct texter_ui_t {
    lv_obj_t * bo;
};

struct texter_ui_convo_t {
    lv_obj_t * obj;
};

static void free_user_data(lv_event_t * e)
{
    free(lv_event_get_user_data(e));
}

texter_ui_t * texter_ui_create(lv_obj_t * base_obj)
{
    texter_ui_t * x = calloc(1, sizeof(*x));
    assert(x);

    x->bo = base_obj;

    lv_obj_add_event_cb(base_obj, free_user_data, LV_EVENT_DELETE, x);

    /* top bar */
    lv_obj_set_flex_flow(base_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_t * top_bar = lv_obj_create(base_obj);
    lv_obj_set_size(top_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(top_bar, 8, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_t * top_label = lv_label_create(top_bar);
    lv_label_set_text_static(top_label, "Texter UI");

    /* convos */
    lv_obj_t * convo_area = lv_obj_create(base_obj);
    lv_obj_remove_style_all(convo_area);
    lv_obj_set_flex_flow(convo_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(convo_area, LV_PCT(100));
    lv_obj_set_flex_grow(convo_area, 1);

    return x;
}

void texter_ui_set_top_text(texter_ui_t * x, const char * text)
{
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(x->bo, 0), 0), text);
}

texter_ui_convo_t * texter_ui_convo_create(texter_ui_t * x)
{
    texter_ui_convo_t * convo = calloc(1, sizeof(*convo));
    assert(convo);

    convo->obj = lv_obj_create(lv_obj_get_child(x->bo, 1));
    lv_obj_set_size(convo->obj, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_add_event_cb(convo->obj, free_user_data, LV_EVENT_DELETE, convo);

    lv_obj_t * title = lv_label_create(convo->obj);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text_static(title, "(untitled)");

    return convo;
}

void texter_ui_convo_set_title(texter_ui_convo_t * convo, const char * text)
{
    lv_label_set_text(lv_obj_get_child(convo->obj, 0), text);
}
