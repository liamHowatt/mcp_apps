#include "bindings.h"

#ifdef CONFIG_MCP_APPS_MCP_LVGL
#include <mcp/mcp_lvgl.h>

const m4_runtime_cb_array_t m4_runtime_lib_mcp_lvgl[] = {
    {"mcp_lvgl_queue_open", {m4_f10, mcp_lvgl_queue_open}},
    {"mcp_lvgl_queue_send", {m4_f02, mcp_lvgl_queue_send}},
    {"mcp_lvgl_queue_close", {m4_f01, mcp_lvgl_queue_close}},

    {NULL}
};

#endif /*CONFIG_MCP_APPS_MCP_LVGL*/
