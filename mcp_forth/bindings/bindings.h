#pragma once

#include "../mcp_forth/mcp_forth.h"
#include <nuttx/config.h>

#ifdef CONFIG_MCP_APPS_MCP_DAEMON
extern const m4_runtime_cb_array_t m4_runtime_lib_mcp_daemon[];
#define M4_RUNTIME_LIB_ENTRY_MCP_DAEMON m4_runtime_lib_mcp_daemon,
#else
#define M4_RUNTIME_LIB_ENTRY_MCP_DAEMON
#endif
