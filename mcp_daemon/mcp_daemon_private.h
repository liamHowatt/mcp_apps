#pragma once

#include <mcp/mcp_daemon.h>

#include <stddef.h>

#define RESULT_OK                 0
#define RESULT_TOKEN_DOESNT_EXIST 1
#define RESULT_MODULE_BUSY        2

#define OPERATION_QUIT            0
#define OPERATION_READ            1
#define OPERATION_WRITE           2

#define SOC_PATH "mcpd"
#define SOC_WAITER_FIFO "mcpd_"

ssize_t mcp_daemon_util_full_read(int fd, void * buf, size_t count);
