#include <mcp/texter_ui.h>

#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO

void texter_ui_demo_app_run(lv_obj_t * base_obj)
{
    texter_ui_t * ui = texter_ui_create(base_obj, "Texter UI Demo");
    (void)ui;
}

#endif /*CONFIG_MCP_APPS_TEXTER_UI_DEMO*/
