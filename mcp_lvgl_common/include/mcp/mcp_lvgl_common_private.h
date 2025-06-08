#pragma once

#include <mcp/mcp_lvgl_common.h>

#include <stdbool.h>

void mcp_lvgl_poll_init(void);
bool mcp_lvgl_poll_run(uint32_t timeout);
void mcp_lvgl_poll_run_until_done(void);
void mcp_lvgl_poll_deinit(void);
