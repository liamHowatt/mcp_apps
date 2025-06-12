#include <mcp/texter_ui.h>

#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO

#include <stdio.h>

void texter_ui_demo_app_run(lv_obj_t * base_obj)
{
    texter_ui_convo_t * convo;

    texter_ui_t * x = texter_ui_create(base_obj);
    texter_ui_set_top_text(x, "Texter UI Demo");

    convo = texter_ui_convo_create(x);
    texter_ui_convo_set_title(convo, "A LoooooooooooOooooooooooooooooOooooooooooooooooooOooooooooooooooooooOoooooooooooooooooong Title");

    for(int i = 1; i < 21; i++) {
        convo = texter_ui_convo_create(x);

        char title[16];
        snprintf(title, sizeof(title), "Convo #%d", i);
        texter_ui_convo_set_title(convo, title);
    }
}

#endif /*CONFIG_MCP_APPS_TEXTER_UI_DEMO*/
