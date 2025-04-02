include $(APPDIR)/Make.defs

# MCP LVGL built-in application info

PROGNAME = $(CONFIG_MCP_APPS_MCP_LVGL_PROGNAME)
PRIORITY = $(CONFIG_MCP_APPS_MCP_LVGL_PRIORITY)
STACKSIZE = $(CONFIG_MCP_APPS_MCP_LVGL_STACKSIZE)
MODULE = $(CONFIG_MCP_APPS_MCP_LVGL)

# MCP LVGL

MAINSRC = mcp_lvgl.c
CSRCS += runtime_lvgl.c

lvgl.json: lv_conf_json.h
	python3 $(APPDIR)/graphics/lvgl/lvgl/scripts/gen_json/gen_json.py --output-path . --lvgl-config lv_conf_json.h

runtime_lvgl.c: runtime_lvgl.h lvgl.json ../mcp_forth/mcp_forth/forth_programs/lvgl/workspace/lvgl_json.jq
	jq -rf ../mcp_forth/mcp_forth/forth_programs/lvgl/workspace/lvgl_json.jq lvgl.json > runtime_lvgl.c

clean::
	$(call DELFILE, lvgl.json)
	$(call DELFILE, runtime_lvgl.c)

distclean:: clean

include $(APPDIR)/Application.mk
