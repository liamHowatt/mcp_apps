#pragma once

#include "../../mcp_forth/mcp_forth.h"
#include "../../bindings/bindings.h"

#define mcp_forth_global_cleanup m4_global_cleanup

typedef enum {
    MCP_FORTH_ERROR_NONE = 0,
    MCP_FORTH_ERROR_PATH_OPEN,
    MCP_FORTH_ERROR_COMPILE,
    MCP_FORTH_ERROR_RUNTIME,
} mcp_forth_error_t;

typedef struct {
    uint8_t * bin;
    void * dl_handle;
    unsigned dl_number;
} mcp_forth_load_t;

typedef struct {
    int open_errno_val;
    int m4_error_val;
    int compile_error_near;
    const char * missing_runtime_word;
} mcp_forth_error_info_t;

mcp_forth_error_t mcp_forth_load_and_run_path(mcp_forth_load_t * load_dst, const char * path,
                                              uint8_t * memory, int memory_len,
                                              const m4_runtime_cb_array_t * const * runtime_cbs,
                                              bool native,
                                              mcp_forth_error_info_t * error_dst);

void mcp_forth_log_error(mcp_forth_error_t load_res, const mcp_forth_error_info_t * load_error);

void mcp_forth_unload(mcp_forth_load_t * load);
