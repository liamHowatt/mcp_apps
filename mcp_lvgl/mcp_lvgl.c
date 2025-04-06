#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>

#include <mcp/mcp_lvgl.h>
#include <mcp/mcp_forth.h>
#include <mcp/mcp_fs.h>

#include <lvgl/lvgl.h>

#include "runtime_lvgl.h"

static mqd_t inner_open(int oflag)
{
    struct mq_attr attr = {.mq_maxmsg = 16, .mq_msgsize = 32};
    mqd_t q = mq_open("mcp_lvgl", oflag, 0666, &attr);
    assert(q != -1);
    return q;
}

mcp_lvgl_queue_t mcp_lvgl_queue_open(void)
{
    return inner_open(O_WRONLY | O_CREAT);
}

void mcp_lvgl_queue_send(mcp_lvgl_queue_t mq, const char * msg)
{
    int res;

    size_t msg_size = strlen(msg) + 1;
    assert(msg_size <= 32);
    res = mq_send(mq, msg, msg_size, 10);
    assert(res == 0);
}

void mcp_lvgl_queue_close(mcp_lvgl_queue_t mqd)
{
    int res = mq_close(mqd);
    assert(res == 0);
}

static void load_forth_driver(const char * path, void ** driver_bin, void ** driver_memory)
{
    int res;
    ssize_t rwres;

    char * cachepath = mcp_fs_cache_file(path);
    if(cachepath) {
        path = cachepath;
    }

    int fd = open(path, O_RDONLY);
    if(fd == -1) {
        perror("open");
        free(cachepath);
        exit(1);
    }

    free(cachepath);

    struct stat st;
    res = fstat(fd, &st);
    assert(res == 0);
    ssize_t buf_len = st.st_size;
    assert(buf_len >= 0);

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
        exit(1);
    }

    static const m4_runtime_cb_array_t * cbs[] = {
        m4_runtime_lib_io,
        m4_runtime_lib_string,
        m4_runtime_lib_time,
        m4_runtime_lib_assert,
        M4_RUNTIME_LIB_MCP_ALL_ENTRIES
        runtime_lib_lvgl,
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
    if(res == M4_RUNTIME_WORD_MISSING_ERROR) {
        fprintf(stderr, "m4_vm_engine_run: runtime word \"%s\" missing\n", missing_word);
        free(memory);
        exit(1);
    }
    if(res) {
        fprintf(stderr, "m4_vm_engine_run: engine error %d\n", res);
        free(memory);
        exit(1);
    }

    *driver_bin = bin;
    *driver_memory = memory;
}

static uint32_t millis(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t tick = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    return tick;
}

int mcp_lvgl_main(int argc, char *argv[])
{
    ssize_t rwres;

    mqd_t mq = inner_open(O_RDONLY | O_CREAT);

    char msg[32];
    rwres = mq_receive(mq, msg, 32, NULL);
    assert(rwres > 0 && rwres <= 32);

    lv_init();
    lv_tick_set_cb(millis);

    void * driver_bin;
    void * driver_memory;
    load_forth_driver(msg, &driver_bin, &driver_memory);

    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_label_set_text_static(label, "Hello Modular Phone!");
    lv_obj_center(label);

    while (1) {
        uint32_t time_til_next = lv_timer_handler();
        if(time_til_next == LV_NO_TIMER_READY) break;
        usleep(time_til_next * 1000);
    }

    lv_deinit();

    free(driver_memory);
    free(driver_bin);

    puts("mcp_lvgl exited");
    return 0;
}
