#include <mcp/texter_ui.h>

struct texter_ui_t {

};

texter_ui_t * texter_ui_create(
    lv_obj_t * base_obj,
    const char * top_text
)
{
    /* top bar */
    lv_obj_set_flex_flow(base_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_t * top_bar = lv_obj_create(base_obj);
    lv_obj_set_size(top_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(top_bar, 8, 0);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(top_bar, 0, 0);
    lv_obj_t * top_label = lv_label_create(top_bar);
    lv_label_set_text(top_label, top_text);

    return NULL;
}
