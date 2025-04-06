#pragma once

#include "../mcp_forth/mcp_forth.h"
#include <nuttx/config.h>

#ifdef CONFIG_MCP_APPS_MCPD
extern const m4_runtime_cb_array_t m4_runtime_lib_mcpd[];
#define M4_RUNTIME_LIB_ENTRY_MCPD m4_runtime_lib_mcpd,
#else
#define M4_RUNTIME_LIB_ENTRY_MCPD
#endif

#ifdef CONFIG_SPI_DRIVER
extern const m4_runtime_cb_array_t m4_runtime_lib_spi[];
#define M4_RUNTIME_LIB_ENTRY_SPI m4_runtime_lib_spi,
#else
#define M4_RUNTIME_LIB_ENTRY_SPI
#endif

#ifdef CONFIG_MCP_APPS_MCP_LVGL
extern const m4_runtime_cb_array_t m4_runtime_lib_mcp_lvgl[];
#define M4_RUNTIME_LIB_ENTRY_MCP_LVGL m4_runtime_lib_mcp_lvgl,
#else
#define M4_RUNTIME_LIB_ENTRY_MCP_LVGL
#endif

extern const m4_runtime_cb_array_t m4_runtime_lib_unix[];
extern const m4_runtime_cb_array_t m4_runtime_lib_malloc[];

#define M4_RUNTIME_LIB_MCP_ALL_ENTRIES \
    M4_RUNTIME_LIB_ENTRY_MCPD \
    M4_RUNTIME_LIB_ENTRY_SPI \
    M4_RUNTIME_LIB_ENTRY_MCP_LVGL \
    m4_runtime_lib_unix, \
    m4_runtime_lib_malloc,
