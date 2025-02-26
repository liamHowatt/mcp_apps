#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef mqd_t mcp_lvgl_queue_t;

mcp_lvgl_queue_t mcp_lvgl_queue_open(void);
bool mcp_lvgl_queue_open_was_ok(mcp_lvgl_queue_t);
void mcp_lvgl_queue_send(mcp_lvgl_queue_t, const char *);
void mcp_lvgl_queue_close(mcp_lvgl_queue_t);

#ifdef __cplusplus
} /*extern "C"*/
#endif
