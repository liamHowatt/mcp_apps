#pragma once

#include <sys/epoll.h>

typedef struct mcp_lvgl_poll_t mcp_lvgl_poll_t;

typedef void (*mcp_lvgl_poll_cb_t)(int fd, uint32_t revents, void * user_data);

mcp_lvgl_poll_t * mcp_lvgl_poll_add(int fd, mcp_lvgl_poll_cb_t cb, uint32_t events, void * user_data);
void mcp_lvgl_poll_modify(mcp_lvgl_poll_t * handle, uint32_t events);
void mcp_lvgl_poll_remove(mcp_lvgl_poll_t * handle);
