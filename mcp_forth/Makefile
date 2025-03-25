include $(APPDIR)/Make.defs

# MCP Forth built-in application info

PROGNAME = $(CONFIG_MCP_APPS_MCP_FORTH_PROGNAME)
PRIORITY = $(CONFIG_MCP_APPS_MCP_FORTH_PRIORITY)
STACKSIZE = $(CONFIG_MCP_APPS_MCP_FORTH_STACKSIZE)
MODULE = $(CONFIG_MCP_APPS_MCP_FORTH)

# MCP Forth

MAINSRC = mcp_forth.c
CSRCS += mcp_forth/mcp_forth.c
CSRCS += mcp_forth/compile.c
CSRCS += mcp_forth/vm_backend.c
CSRCS += mcp_forth/vm_engine.c
CSRCS += mcp_forth/runtime_io.c
CSRCS += mcp_forth/runtime_string.c
CSRCS += mcp_forth/runtime_time.c
CSRCS += bindings/runtime_mcpd.c

include $(APPDIR)/Application.mk
