#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <poll.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/touchscreen.h>
#include <errno.h>

#include <mcp/mcp_lvgl.h>
#include <mcp/mcp_forth.h>
#include <mcp/mcp_fs.h>

#include <lvgl/lvgl.h>

#include "runtime_lvgl.h"

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} keypad_data_t;

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

static void make_enc_kpd_group(void)
{
    lv_group_t * g = lv_group_create();
    lv_group_set_default(g);

    lv_indev_t * indev = NULL;
    while((indev = lv_indev_get_next(indev))) {
        lv_indev_set_group(indev, g);
    }
}

// static void indev_cb(lv_indev_t * indev, lv_indev_data_t * data)
// {
//     data->key = LV_KEY_DOWN;

//     uint32_t tick = lv_tick_get();
//     static uint32_t next;
//     static bool toggle;
//     if(tick > next) {
//         next = tick + 200;
//         toggle = !toggle;
//     }
//     data->state = toggle ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
// }

static void keypad_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    keypad_data_t * keypad_data = lv_indev_get_user_data(indev);
    data->key = keypad_data->key;
    data->state = keypad_data->state;
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

    make_enc_kpd_group();

    lv_obj_t * scr = lv_screen_active();

    lv_obj_t * list = lv_list_create(scr);
    lv_gridnav_add(list, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(lv_group_get_default(), list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    for(int i = 1; i <= 20; i++) {
        char buf[3];
        sprintf(buf, "%d", i);
        lv_obj_t * btn = lv_list_add_button(list, NULL, buf);
        lv_group_remove_obj(btn);
    }

    int keypad_fd = open("/dev/ukeyboard", O_RDONLY | O_NONBLOCK);
    assert(keypad_fd >= 0);

    struct pollfd pfd[1] = {{.fd = keypad_fd, .events = POLLIN}};

    lv_indev_t * keypad_indev = NULL;
    keypad_data_t keypad_data;

    while (1) {
        uint32_t time_til_next = lv_timer_handler();
        int timeout = time_til_next == LV_NO_TIMER_READY ? -1 : time_til_next;
        int event_count = poll(pfd, sizeof(pfd) / sizeof(*pfd), timeout);
        assert(event_count >= 0);
        if(!event_count) continue;

        assert(pfd[0].revents == POLLIN);
        if(keypad_indev == NULL) {
            keypad_indev = lv_indev_create();
            lv_indev_set_type(keypad_indev, LV_INDEV_TYPE_KEYPAD);
            lv_indev_set_read_cb(keypad_indev, keypad_cb);
            lv_indev_set_group(keypad_indev, lv_group_get_default());
            lv_indev_set_mode(keypad_indev, LV_INDEV_MODE_EVENT);
            lv_indev_set_user_data(keypad_indev, &keypad_data);
        }
        bool once = false;
        struct keyboard_event_s keypad_event;
        while(sizeof(keypad_event) == (rwres = read(keypad_fd, &keypad_event, sizeof(keypad_event)))) {
            once = true;
            switch(keypad_event.code) {
                case 103: /* KEY_UP */
                    keypad_data.key = LV_KEY_UP;
                    break;
                case 108: /* KEY_DOWN */
                    keypad_data.key = LV_KEY_DOWN;
                    break;
                default:
                    continue;
            }
            switch(keypad_event.type) {
                case KEYBOARD_PRESS:
                    keypad_data.state = LV_INDEV_STATE_PRESSED;
                    break;
                case KEYBOARD_RELEASE:
                    keypad_data.state = LV_INDEV_STATE_RELEASED;
                    break;
                default:
                    continue;
            }
            lv_indev_read(keypad_indev);
        }
        assert(rwres < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        assert(once);

    }

    lv_deinit();

    free(driver_memory);
    free(driver_bin);

    assert(0 == close(keypad_fd));

    mcp_lvgl_queue_close(mq);

    puts("mcp_lvgl exited");
    return 0;
}
