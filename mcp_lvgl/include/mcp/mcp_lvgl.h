#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <mqueue.h>
#include <stdbool.h>

typedef mqd_t mcp_lvgl_queue_t;

mcp_lvgl_queue_t mcp_lvgl_queue_open(void);
void mcp_lvgl_queue_send(mcp_lvgl_queue_t, const char *);
void mcp_lvgl_queue_close(mcp_lvgl_queue_t);

#ifdef __cplusplus
} /*extern "C"*/
#endif
