#include "beeper_ui_private.h"

static void timer_cb(lv_timer_t * t)
{
    LV_IMAGE_DECLARE(beeper_ui_img_fb_messenger_64);
    lv_obj_t * base_obj = lv_timer_get_user_data(t);
    lv_obj_t * img = lv_image_create(base_obj);
    lv_image_set_src(img, &beeper_ui_img_fb_messenger_64);
}

void beeper_ui_networks(lv_obj_t * base_obj)
{
    lv_obj_set_flex_flow(base_obj, LV_FLEX_FLOW_ROW_WRAP);
    lv_timer_t * tim = lv_timer_create(timer_cb, 500, base_obj);
    lv_timer_set_repeat_count(tim, 6);
    lv_timer_set_auto_delete(tim, true);
}
