#include "bindings.h"

#ifdef CONFIG_MCP_APPS_MCP_DAEMON
#include "mcp/mcp_daemon.h"
#include <stdlib.h>
#include <stdint.h>

#define MCP_DAEMON_DRIVER_ENV_NOT_SET       -1000
#define MCP_DAEMON_DRIVER_PROTOCOL_NOT_SUP  -1001

static int mcp_daemon_driver_connect(void * param, m4_stack_t * stack)
{
    if(!(stack->len + 2 <= stack->max)) return M4_STACK_OVERFLOW_ERROR;
    stack->data += 2;
    stack->len += 2;

    int res;
    char * peer_str = getenv("MCP_PEER");
    if(peer_str == NULL) {
        stack->data[-1] = MCP_DAEMON_DRIVER_ENV_NOT_SET;
        return 0;
    }
    int peer = atoi(peer_str);
    mcp_daemon_con_t con;
    res = mcp_daemon_connect(&con, peer);
    if(res != MCP_DAEMON_OK) {
        stack->data[-1] = res;
        return 0;
    }
    uint8_t byte = 1; /* protocol 1: the driver protocol */
    mcp_daemon_write(con, &byte, 1);
    mcp_daemon_read(con, &byte, 1);
    if(byte != 0) {
        mcp_daemon_disconnect(con);
        stack->data[-1] = MCP_DAEMON_DRIVER_PROTOCOL_NOT_SUP;
        return 0;
    }
    stack->data[-2] = (int) con;
    stack->data[-1] = MCP_DAEMON_OK;
    return 0;
}

const m4_runtime_cb_array_t m4_runtime_lib_mcp_daemon[] = {
    {"mcp_daemon_connect", {m4_f12, mcp_daemon_connect}},
    {"mcp_daemon_disconnect", {m4_f01, mcp_daemon_disconnect}},
    {"mcp_daemon_write", {m4_f03, mcp_daemon_write}},
    {"mcp_daemon_read", {m4_f03, mcp_daemon_read}},
    {"mcp_daemon_ok", {m4_lit, (void *) (MCP_DAEMON_OK)}},
    {"mcp_daemon_doesnt_exist", {m4_lit, (void *) (MCP_DAEMON_DOESNT_EXIST)}},
    {"mcp_daemon_busy", {m4_lit, (void *) (MCP_DAEMON_BUSY)}},
    {"mcp_daemon_con_null", {m4_lit, (void *) (MCP_DAEMON_CON_NULL)}},

    /*extensions*/
    {"mcp_daemon_driver_connect", {mcp_daemon_driver_connect}},
    {"mcp_daemon_driver_env_not_set", {m4_lit, (void *) (MCP_DAEMON_DRIVER_ENV_NOT_SET)}},
    {"mcp_daemon_driver_protocol_not_sup", {m4_lit, (void *) (MCP_DAEMON_DRIVER_PROTOCOL_NOT_SUP)}},

    {NULL}
};

#endif /*CONFIG_MCP_APPS_MCP_DAEMON*/
