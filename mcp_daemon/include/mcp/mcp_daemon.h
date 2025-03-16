#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define MCP_DAEMON_OK            0
#define MCP_DAEMON_DOESNT_EXIST -1
#define MCP_DAEMON_BUSY         -2

#define MCP_DAEMON_CON_NULL     -1

typedef int mcp_daemon_con_t;

int mcp_daemon_connect(mcp_daemon_con_t * con_dst, int peer_id);
void mcp_daemon_disconnect(mcp_daemon_con_t con);
void mcp_daemon_write(mcp_daemon_con_t con, const void * data, uint32_t len);
void mcp_daemon_read(mcp_daemon_con_t con, void * data, uint32_t len);

#ifdef __cplusplus
} /*extern "C"*/
#endif
