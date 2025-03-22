#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "mcp_forth/mcp_forth.h"
#include "bindings/bindings.h"

int mcp_forth_main(int argc, char *argv[])
{
    if(argc != 2) {
        fprintf(stderr, "usage: mcp_forth <file path>\n");
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if(fd == -1) {
        perror("open");
        return 1;
    }

    char * buf = NULL;
    size_t buf_len = 0;
    size_t buf_cap = 0;
    while(1) {
        if(buf_len == buf_cap) {
            buf_cap += 256;
            buf = realloc(buf, buf_cap);
            assert(buf);
        }
        size_t read_amt = buf_cap - buf_len;
        ssize_t br = read(fd, buf + buf_len, read_amt);
        if(br < 0) {
            perror("read");
            free(buf);
            return 1;
        }
        if(br == 0) {
            break;
        }
        buf_len += br;
    }

    close(fd);

    buf = realloc(buf, buf_len);

    uint8_t * bin;
    int error_near;
    int bin_len = m4_compile(buf, buf_len, &bin,
        &m4_compact_bytecode_vm_backend, &error_near);
    free(buf);
    if(bin_len < 0) {
        fprintf(stderr, "m4_compile: error %d near %d\n", bin_len, error_near);
        return 1;
    }

    static const m4_runtime_cb_array_t * cbs[] = {
        m4_runtime_lib_io,
        m4_runtime_lib_string,
        m4_runtime_lib_time,
        M4_RUNTIME_LIB_ENTRY_MCP_DAEMON
        NULL
    };

    uint8_t * memory = malloc(2048);
    assert(memory);
    const char * missing_word;
    int res = m4_vm_engine_run(
        bin,
        bin_len,
        memory,
        2048,
        cbs,
        &missing_word
    );
    free(memory);
    free(bin);
    if(res == M4_RUNTIME_WORD_MISSING_ERROR) {
        fprintf(stderr, "m4_vm_engine_run: runtime word \"%s\" missing\n", missing_word);
        return 1;
    }
    if(res) {
        fprintf(stderr, "m4_vm_engine_run: engine error %d\n", res);
        return 1;
    }

    return 0;
}
