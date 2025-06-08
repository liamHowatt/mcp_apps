#include <mcp/beeper.h>
#include "beeper_ui_private.h"

void beeper_app_run(lv_obj_t * base_obj)
{
    int res = mkdir(BEEPER_ROOT_PATH, 0755);
    assert(res == 0 || errno == EEXIST);

    beeper_ui_base_obj_init(base_obj);
    beeper_ui_login(base_obj);
}
