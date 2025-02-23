include $(APPDIR)/Make.defs

# MCP Daemon built-in application info

PROGNAME = $(CONFIG_MCP_APPS_MCP_DAEMON_PROGNAME)
PRIORITY = $(CONFIG_MCP_APPS_MCP_DAEMON_PRIORITY)
STACKSIZE = $(CONFIG_MCP_APPS_MCP_DAEMON_STACKSIZE)
MODULE = $(CONFIG_MCP_APPS_MCP_DAEMON)

# MCP Daemon

MAINSRC = mcp_daemon.c
CSRCS += mcp_board/mcp_bitbang/mcp_bitbang_client.c

include $(APPDIR)/Application.mk
