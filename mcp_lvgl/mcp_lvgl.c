#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/stat.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/touchscreen.h>
#include <errno.h>

#include <mcp/mcp_lvgl.h>
#include <mcp/mcp_lvgl_common_private.h>
#include <mcp/mcp_forth.h>
#include <mcp/mcpd.h>
#include <mcp/mcp_fs.h>

#ifdef CONFIG_MCP_APPS_PEANUT_GB
#include <mcp/peanut_gb.h>
#endif
#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO
#include <mcp/texter_ui.h>
#endif

#include <lvgl/lvgl.h>
#include <lvgl/src/core/lv_global.h>

#include "runtime_lvgl.h"

#define MQ_MSGSIZE 64
#define FORTH_DRIVER_MEMORY_SIZE 2048

typedef void (*app_cb_t)(lv_obj_t * base_obj);

typedef struct {
    const char * name;
    app_cb_t cb;
} app_entry_t;

typedef struct {
    int app_count;
    app_entry_t * app_entries;
    lv_obj_t * app_list_obj;
} lvgl_user_data_t;

typedef struct {
    uint8_t * bin;
    uint8_t memory[FORTH_DRIVER_MEMORY_SIZE];
} forth_driver_t;

typedef struct driver_ll_s {
    struct driver_ll_s * next;
    forth_driver_t drv;
} driver_ll_t;

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} keypad_data_t;

typedef struct {
    lv_indev_t * keypad_indev;
    keypad_data_t keypad_data;
} keypad_poll_data_t;

typedef void (*mcp_lvgl_async_cb_t)(void * user_data);

typedef struct {
    mcpd_con_t con;
    mcp_lvgl_async_cb_t cb;
    void * user_data;
    uint32_t cur_events;
} mcp_lvgl_async_t;

static const m4_runtime_cb_array_t runtime_lib_lvgl_common[] = {
    {"mcp_lvgl_poll_add", {m4_f14, mcp_lvgl_poll_add}},
    {"mcp_lvgl_poll_modify", {m4_f02, mcp_lvgl_poll_modify}},
    {"mcp_lvgl_poll_remove", {m4_f01, mcp_lvgl_poll_remove}},
    {NULL}
};

static mqd_t inner_open(int oflag)
{
    struct mq_attr attr = {.mq_maxmsg = 16, .mq_msgsize = MQ_MSGSIZE};
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
    assert(msg_size <= MQ_MSGSIZE);
    res = mq_send(mq, msg, msg_size, 10);
    assert(res == 0);
}

void mcp_lvgl_queue_close(mcp_lvgl_queue_t mqd)
{
    int res = mq_close(mqd);
    assert(res == 0);
}

static void app_clicked_cb(lv_event_t * e);
static void add_entry_to_app_list_obj(lv_obj_t * list, const app_entry_t * entry);

static void create_app_list(void)
{
    lvgl_user_data_t * ud = LV_GLOBAL_DEFAULT()->user_data;
    lv_obj_t * list = lv_list_create(lv_screen_active());
    ud->app_list_obj = list;
    lv_gridnav_add(list, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(lv_group_get_default(), list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_t * btn;
    (void)btn;
#ifdef CONFIG_MCP_APPS_PEANUT_GB
    btn = lv_list_add_button(list, NULL, "Peanut GB");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, peanut_gb_app_run);
#endif
#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO
    btn = lv_list_add_button(list, NULL, "Texter UI Demo");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, texter_ui_demo_app_run);
#endif
    for(int i = 0; i < ud->app_count; i++) {
        add_entry_to_app_list_obj(list, &ud->app_entries[i]);
    }
}

static void app_obj_delete_cb(lv_event_t * e)
{
    create_app_list();
}

static void app_clicked_cb(lv_event_t * e)
{
    lv_obj_clean(lv_screen_active());
    lvgl_user_data_t * ud = LV_GLOBAL_DEFAULT()->user_data;
    ud->app_list_obj = NULL;
    lv_obj_t * base_obj = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(base_obj);
    lv_obj_set_size(base_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(base_obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(base_obj, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(base_obj, app_obj_delete_cb, LV_EVENT_DELETE, NULL);
    app_cb_t app_cb = lv_event_get_user_data(e);
    app_cb(base_obj);
}

static void add_entry_to_app_list_obj(lv_obj_t * list, const app_entry_t * entry)
{
    lv_obj_t * btn = lv_list_add_button(list, NULL, entry->name);
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, entry->cb);
}

static int mcp_lvgl_app_register(void * param, m4_stack_t * stack)
{
    if(!(stack->len >= 2)) return M4_STACK_UNDERFLOW_ERROR;
    lvgl_user_data_t * ud = LV_GLOBAL_DEFAULT()->user_data;
    ud->app_entries = realloc(ud->app_entries, ++ud->app_count);
    assert(ud->app_entries);
    ud->app_entries[ud->app_count - 1].name = (const char *) stack->data[-2];
    ud->app_entries[ud->app_count - 1].cb   = (app_cb_t)     stack->data[-1];
    if(ud->app_list_obj) {
        add_entry_to_app_list_obj(ud->app_list_obj, &ud->app_entries[ud->app_count - 1]);
    }
    stack->data -= 2;
    stack->len -= 2;
    return 0;
}

static const m4_runtime_cb_array_t runtime_lib_lvgl_app[] = {
    {"mcp_lvgl_app_register", {mcp_lvgl_app_register}},
    {NULL}
};

static void async_poll_cb(mcp_lvgl_poll_t * handle, int fd, uint32_t revents, void * user_data)
{
    mcp_lvgl_async_t * a = user_data;
    assert(revents == a->cur_events);

    int res = mcpd_async_continue(a->con);

    if(res == MCPD_OK) {
        mcp_lvgl_poll_remove(handle);
        a->cb(a->user_data);
        free(a);
        return;
    }

    uint32_t events;
    if(res == MCPD_ASYNC_WANT_WRITE) {
        events = EPOLLOUT;
    } else if(res == MCPD_ASYNC_WANT_READ) {
        events = EPOLLIN;
    } else assert(0);

    if(events != a->cur_events) {
        a->cur_events = events;
        mcp_lvgl_poll_modify(handle, events);
    }
}

static void mcp_lvgl_async_mcpd_inner(int mcpd_need, mcpd_con_t con,
                                      mcp_lvgl_async_cb_t cb, void * user_data)
{
    uint32_t events;
    if(mcpd_need == MCPD_ASYNC_WANT_WRITE) {
        events = EPOLLOUT;
    } else if(mcpd_need == MCPD_ASYNC_WANT_READ) {
        events = EPOLLIN;
    } else assert(0);
    int fd = mcpd_get_async_polling_fd(con);
    mcp_lvgl_async_t * a = malloc(sizeof(*a));
    assert(a);
    a->con = con;
    a->cb = cb;
    a->user_data = user_data;
    a->cur_events = events;
    mcp_lvgl_poll_add(fd, async_poll_cb, events, a);
}

static void mcp_lvgl_async_mcpd_write(mcpd_con_t con, const void * src, uint32_t len,
                                      mcp_lvgl_async_cb_t cb, void * user_data)
{
    mcp_lvgl_async_mcpd_inner(mcpd_async_write_start(con, src, len), con, cb, user_data);
}

static void mcp_lvgl_async_mcpd_read(mcpd_con_t con, void * dst, uint32_t len,
                                     mcp_lvgl_async_cb_t cb, void * user_data)
{
    mcp_lvgl_async_mcpd_inner(mcpd_async_read_start(con, dst, len), con, cb, user_data);
}

static const m4_runtime_cb_array_t runtime_lib_lvgl_async_mcpd[] = {
    {"mcp_lvgl_async_mcpd_write", {m4_f05, mcp_lvgl_async_mcpd_write}},
    {"mcp_lvgl_async_mcpd_read", {m4_f05, mcp_lvgl_async_mcpd_read}},
    {NULL}
};

static void load_forth_driver(forth_driver_t * drv, const char * path)
{
    int res;
    ssize_t rwres;

    int id = mcp_fs_path_get_peer_id(path);
    if(id >= 0) {
        char id_str[4];
        res = snprintf(id_str, sizeof(id_str), "%d", id);
        assert(res > 0 && res < sizeof(id_str));
        res = setenv("MCP_PEER", id_str, 1);
        assert(res == 0);
    }

    char * cachepath = mcp_fs_cache_file(path);
    if(cachepath) {
        path = cachepath;
    }

    int fd = open(path, O_RDONLY);
    free(cachepath);
    if(fd == -1) {
        perror("open");
        exit(1);
    }

    struct stat st;
    res = fstat(fd, &st);
    assert(res == 0);
    ssize_t buf_len = st.st_size;
    assert(buf_len >= 0);

    char * code_buf = malloc(buf_len);
    assert(code_buf);
    rwres = read(fd, code_buf, buf_len);
    assert(rwres == buf_len);

    res = close(fd);
    assert(res == 0);

    int error_near;
    int bin_len = m4_compile(code_buf, buf_len, &drv->bin,
        &m4_compact_bytecode_vm_backend, &error_near);
    free(code_buf);
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
        runtime_lib_lvgl_app,
        runtime_lib_lvgl_common,
        runtime_lib_lvgl_async_mcpd,
        NULL
    };

    const char * missing_word;
    res = m4_vm_engine_run(
        drv->bin,
        bin_len,
        drv->memory,
        FORTH_DRIVER_MEMORY_SIZE,
        cbs,
        &missing_word
    );
    if(res == M4_RUNTIME_WORD_MISSING_ERROR) {
        fprintf(stderr, "m4_vm_engine_run: runtime word \"%s\" missing\n", missing_word);
        free(drv->bin);
        exit(1);
    }
    if(res) {
        fprintf(stderr, "m4_vm_engine_run: engine error %d\n", res);
        free(drv->bin);
        exit(1);
    }
}

static void unload_forth_driver(forth_driver_t * drv)
{
    free(drv->bin);
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

static void add_driver(driver_ll_t ** drv_head_p, const char * path)
{
    driver_ll_t * new_head = malloc(sizeof(*new_head));
    assert(new_head);
    new_head->next = *drv_head_p;
    load_forth_driver(&new_head->drv, path);
    *drv_head_p = new_head;
}

static void mq_poll_cb(mcp_lvgl_poll_t * handle, int fd, uint32_t revents, void * user_data)
{
    assert(revents == EPOLLIN);

    ssize_t rwres;

    mqd_t mq = fd;
    driver_ll_t ** drv_head_p = user_data;

    bool once = false;
    char msg[MQ_MSGSIZE];
    while (0 <= (rwres = mq_receive(mq, msg, MQ_MSGSIZE, NULL))) {
        once = true;
        assert(rwres > 0 && rwres <= MQ_MSGSIZE);
        add_driver(drv_head_p, msg);
    }
    assert(errno == EAGAIN);
    assert(once);
}

static void keypad_poll_cb(mcp_lvgl_poll_t * handle, int fd, uint32_t revents, void * user_data)
{
    assert(revents == EPOLLIN);

    ssize_t rwres;

    keypad_poll_data_t * d = user_data;

    if(d->keypad_indev == NULL) {
        d->keypad_indev = lv_indev_create();
        lv_indev_set_type(d->keypad_indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_read_cb(d->keypad_indev, keypad_cb);
        lv_indev_set_group(d->keypad_indev, lv_group_get_default());
        lv_indev_set_mode(d->keypad_indev, LV_INDEV_MODE_EVENT);
        lv_indev_set_user_data(d->keypad_indev, &d->keypad_data);
    }
    bool once = false;
    struct keyboard_event_s keypad_event;
    while(sizeof(keypad_event) == (rwres = read(fd, &keypad_event, sizeof(keypad_event)))) {
        once = true;
        switch(keypad_event.code) {
            case 103: /* KEY_UP */
                d->keypad_data.key = LV_KEY_UP;
                break;
            case 108: /* KEY_DOWN */
                d->keypad_data.key = LV_KEY_DOWN;
                break;
            case 105: /* KEY_LEFT */
                d->keypad_data.key = LV_KEY_LEFT;
                break;
            case 106: /* KEY_RIGHT */
                d->keypad_data.key = LV_KEY_RIGHT;
                break;
            case 1: /* KEY_ESC */
                d->keypad_data.key = LV_KEY_ESC;
                break;
            case 28: /* KEY_ENTER */
                d->keypad_data.key = LV_KEY_ENTER;
                break;
            default:
                continue;
        }
        switch(keypad_event.type) {
            case KEYBOARD_PRESS:
                d->keypad_data.state = LV_INDEV_STATE_PRESSED;
                break;
            case KEYBOARD_RELEASE:
                d->keypad_data.state = LV_INDEV_STATE_RELEASED;
                break;
            default:
                continue;
        }
        lv_indev_read(d->keypad_indev);
    }
    assert(rwres < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    assert(once);
}

int mcp_lvgl_main(int argc, char *argv[])
{
    ssize_t rwres;

    lvgl_user_data_t lvgl_user_data = {0};

    driver_ll_t * drv_head = NULL;

    mqd_t mq = inner_open(O_RDONLY | O_CREAT);
    char msg[MQ_MSGSIZE];

    bool lvgl_is_init = false;
    do {
        rwres = mq_receive(mq, msg, MQ_MSGSIZE, NULL);
        assert(rwres > 0 && rwres <= MQ_MSGSIZE);

        if(!lvgl_is_init) {
            lvgl_is_init = true;
            lv_init();
            LV_GLOBAL_DEFAULT()->user_data = &lvgl_user_data;
            mcp_lvgl_poll_init();
        }

        add_driver(&drv_head, msg);

    } while(!lv_display_get_default());

    lv_tick_set_cb(millis);
    make_enc_kpd_group();

    create_app_list();

    int flags = fcntl(mq, F_GETFL, 0);
    assert(flags != -1);
    assert(-1 != fcntl(mq, F_SETFL, flags | O_NONBLOCK));
    mcp_lvgl_poll_add(mq, mq_poll_cb, EPOLLIN, &drv_head);

    int keypad_fd = open("/dev/ukeyboard", O_RDONLY | O_NONBLOCK);
    assert(keypad_fd >= 0);
    keypad_poll_data_t keypad_poll_data;
    keypad_poll_data.keypad_indev = NULL;
    mcp_lvgl_poll_add(keypad_fd, keypad_poll_cb, EPOLLIN, &keypad_poll_data);

    mcp_lvgl_poll_run_until_done();

    mcp_lvgl_poll_deinit();
    lv_deinit();

    do {
        unload_forth_driver(&drv_head->drv);
        driver_ll_t * next = drv_head->next;
        free(drv_head);
        drv_head = next;
    } while(drv_head);

    free(lvgl_user_data.app_entries);

    m4_vm_engine_global_cleanup();

    assert(0 == close(keypad_fd));

    mcp_lvgl_queue_close(mq);

    puts("mcp_lvgl exited");
    return 0;
}
