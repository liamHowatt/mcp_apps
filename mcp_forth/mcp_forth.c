#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mcp_forth/mcp_forth.h"
#include "bindings/bindings.h"

int mcp_forth_main(int argc, char *argv[])
{
    int res;
    ssize_t rwres;

    if(argc != 2) {
        fprintf(stderr, "usage: mcp_forth <file path>\n");
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if(fd == -1) {
        perror("open");
        return 1;
    }

    struct stat st;
    res = fstat(fd, &st);
    assert(res == 0);
    ssize_t buf_len = st.st_size;
    assert(buf_len > 0);

    char * buf = malloc(buf_len);
    assert(buf);
    rwres = read(fd, buf, buf_len);
    assert(rwres == buf_len);

    res = close(fd);
    assert(res == 0);

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
        m4_runtime_lib_assert,
        M4_RUNTIME_LIB_ENTRY_MCPD
        M4_RUNTIME_LIB_ENTRY_SPI
        m4_runtime_lib_unix,
        m4_runtime_lib_malloc,
        NULL
    };

    uint8_t * memory = malloc(2048);
    assert(memory);
    const char * missing_word;
    res = m4_vm_engine_run(
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
