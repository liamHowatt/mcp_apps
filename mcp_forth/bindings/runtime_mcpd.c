#include "bindings.h"

#ifdef CONFIG_MCP_APPS_MCPD
#include "mcp/mcpd.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

static int mcpd_driver_connect(void * param, m4_stack_t * stack)
{
    if(!(stack->len + 2 <= stack->max)) return M4_STACK_OVERFLOW_ERROR;
    stack->data += 2;
    stack->len += 2;

    int res;
    char * peer_str = getenv("MCP_PEER");
    if(peer_str == NULL) {
        stack->data[-1] = MCPD_ENV_NOT_SET;
        return 0;
    }
    int peer = atoi(peer_str);
    mcpd_con_t con;
    res = mcpd_connect(&con, peer);
    if(res != MCPD_OK) {
        stack->data[-1] = res;
        return 0;
    }
    uint8_t byte = 1; /* protocol 1: the driver protocol */
    mcpd_write(con, &byte, 1);
    mcpd_read(con, &byte, 1);
    if(byte != 0) {
        mcpd_disconnect(con);
        stack->data[-1] = MCPD_PROTOCOL_NOT_SUP;
        return 0;
    }
    stack->data[-2] = (int) con;
    stack->data[-1] = MCPD_OK;
    return 0;
}

static int mcpd_peer_id(void * param, m4_stack_t * stack)
{
    if(!(stack->len < stack->max)) return M4_STACK_OVERFLOW_ERROR;
    char * peer_str = getenv("MCP_PEER");
    stack->data[0] = peer_str !=  NULL ? atoi(peer_str) : MCPD_ENV_NOT_SET;
    stack->data += 1;
    stack->len += 1;
    return 0;
}

static int mcpd_acatpath(void * param, m4_stack_t * stack)
{
    if(!(stack->len)) return M4_STACK_UNDERFLOW_ERROR;
    char ** strp = (char **) stack->data - 1;
    char * peer_str = getenv("MCP_PEER");
    if(peer_str == NULL) {
        *strp = NULL;
        return 0;
    }
    int res = asprintf(strp, "/mnt/mcp/%d/%s", atoi(peer_str), *strp);
    assert(res != -1);
    return 0;
}

const m4_runtime_cb_array_t m4_runtime_lib_mcpd[] = {
    {"mcpd_ok", {m4_lit, (void *) (MCPD_OK)}},
    {"mcpd_doesnt_exist", {m4_lit, (void *) (MCPD_DOESNT_EXIST)}},
    {"mcpd_busy", {m4_lit, (void *) (MCPD_BUSY)}},
    {"mcpd_resource_unavailable", {m4_lit, (void *) (MCPD_RESOURCE_UNAVAILABLE)}},
    {"mcpd_bad_request", {m4_lit, (void *) (MCPD_BAD_REQUEST)}},
    {"mcpd_env_not_set", {m4_lit, (void *) (MCPD_ENV_NOT_SET)}},
    {"mcpd_protocol_not_sup", {m4_lit, (void *) (MCPD_PROTOCOL_NOT_SUP)}},
    {"mcpd_ioerror", {m4_lit, (void *) (MCPD_IOERROR)}},
    {"mcpd_noent", {m4_lit, (void *) (MCPD_NOENT)}},
    {"mcpd_nametoolong", {m4_lit, (void *) (MCPD_NAMETOOLONG)}},
    {"mcpd_con_null", {m4_lit, (void *) (MCPD_CON_NULL)}},

    {"mcpd_connect", {m4_f12, mcpd_connect}},
    {"mcpd_disconnect", {m4_f01, mcpd_disconnect}},

    {"mcpd_write", {m4_f03, mcpd_write}},
    {"mcpd_read", {m4_f03, mcpd_read}},

    {"mcpd_gpio_acquire", {m4_f13, mcpd_gpio_acquire}},
    {"mcpd_gpio_set", {m4_f03, mcpd_gpio_set}},

    {"mcpd_resource_acquire", {m4_f12, mcpd_resource_acquire}},
    {"mcpd_resource_route", {m4_f15, mcpd_resource_route}},
    {"mcpd_resource_get_path", {m4_f12, mcpd_resource_get_path}},

    {"mcpd_file_hash", {m4_f13, mcpd_file_hash}},


    /*extensions*/
    {"mcpd_driver_connect", {mcpd_driver_connect}},
    {"mcpd_peer_id", {mcpd_peer_id}},
    {"mcpd_acatpath", {mcpd_acatpath}},


    {NULL}
};

#endif /*CONFIG_MCP_APPS_MCPD*/
