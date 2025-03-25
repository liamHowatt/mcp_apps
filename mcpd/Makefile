include $(APPDIR)/Make.defs

# MCP Daemon built-in application info

PROGNAME = $(CONFIG_MCP_APPS_MCPD_PROGNAME)
PRIORITY = $(CONFIG_MCP_APPS_MCPD_PRIORITY)
STACKSIZE = $(CONFIG_MCP_APPS_MCPD_STACKSIZE)
MODULE = $(CONFIG_MCP_APPS_MCPD)

# MCP Daemon

MAINSRC = mcpd.c
CSRCS += mcpd_lib.c
CSRCS += mcp_board/mcp_bitbang/mcp_bitbang_client.c

include $(APPDIR)/Application.mk
