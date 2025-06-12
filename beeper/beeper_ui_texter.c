#include "beeper_ui_private.h"

void beeper_ui_texter(lv_obj_t * base_obj)
{
    beeper_ui_t * c = lv_obj_get_user_data(base_obj);

    lv_obj_remove_flag(c->x_obj, LV_OBJ_FLAG_HIDDEN);
}
