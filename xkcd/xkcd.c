/*********************
 *      INCLUDES
 *********************/

#include <nuttx/config.h>

#include <mcp/xkcd.h>

#include <mcp/tls.h>
#include <mcp/mcp_lvgl_common.h>

#include <lvgl/lvgl.h>
#include <netutils/webclient.h>
#include <netutils/cJSON.h>
#include <png.h>

#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <math.h> // isnan
#include <stdio.h>
#include <setjmp.h>

/*********************
 *      DEFINES
 *********************/

#define HTTP_CLIENT_BUFSZ (8 * 1024) // industry standard
#define RAW_JSON_BUFSZ (2 * 1024) // largest I saw was 524
#define MAX_DECODED_IMG_BYTES (500 * 1000) // ~707x707 img

#define COMIC_NUM_REQUEST_NONE 0
#define COMIC_NUM_REQUEST_LATEST -1
#define COMIC_NUM_REQUEST_QUIT -2

/**********************
 *      TYPEDEFS
 **********************/

typedef enum {
    RESULT_SUCCESS = 0,
    RESULT_NET_ERROR,
    RESULT_JSON_TOO_BIG_ERROR,
    RESULT_IMAGE_FORMAT_ERROR,
    RESULT_IMAGE_TOO_BIG_ERROR,
    RESULT_JSON_API_ERROR,
    RESULT_PNG_ERROR,
    RESULT_MEM_ALLOC_ERROR,
} result_t;

enum {
    CHILD_TITLE = 0,
    CHILD_NUM_LABEL,
    CHILD_LEFT_BTN,
    CHILD_RIGHT_BTN,
    CHILD_RETRY_BTN,
    CHILD_EXIT_BTN,
    CHILD_CANVAS_BOX,
};

typedef struct {
    // app's data
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    mcp_lvgl_poll_t * pollp;
    lv_obj_t * base_obj;
    bool waiting;
    uint32_t latest_comic_num;
    uint32_t displayed_comic_num;
    int reader;
    int writer; // used by thread, int value immutable

    // mutable state shared with thread
    int32_t shared_comic_num_request;
    result_t shared_result;
    uint32_t shared_comic_num;
    char * shared_title;
    uint8_t * shared_bitmap;
    uint32_t shared_width;
    uint32_t shared_height;

    // thread's data
    uint32_t comic_num;
    result_t specific_error;
    char * title;
    uint8_t * bitmap;
    cJSON * json;
    mcp_tls_ctx_t * tls_ctx;
    struct webclient_context wctx;
    png_structp pngp;
    png_infop infop;
    png_uint_32 width;
    png_uint_32 height;
    union {
        struct { // json request phase
            size_t raw_json_i;
            char urlbuf[sizeof("https://xkcd.com/2147483647/info.0.json")];
            char raw_json_buf[RAW_JSON_BUFSZ];
        } j;
        struct { // image request phase
            bool end_reached;
        } p;
    };
    char client_buf[HTTP_CLIENT_BUFSZ];
} ctx_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

// Common ---------------------------------------------------------------------
static void mutex_lock(pthread_mutex_t * mutex);
static void mutex_unlock(pthread_mutex_t * mutex);
static const char * result_string(result_t result);
static void create_single_byte_pipe(int * reader, int * writer);
static void fd_set_nonblocking(int fd);
// App ------------------------------------------------------------------------
static void poll_cb(mcp_lvgl_poll_t * handle, int fd,
    uint32_t revents, void * user_data);
static void left_btn_cb(lv_event_t * e);
static void right_btn_cb(lv_event_t * e);
static void retry_btn_cb(lv_event_t * e);
static void exit_btn_cb(lv_event_t * e);
static void btn_request_helper(lv_event_t * e, int delta);
static void send_request(ctx_t * c, int32_t num);
// App UI ---------------------------------------------------------------------
static void ui_create(lv_obj_t * base_obj, void * user_data);
static void ui_create_btn_helper(lv_obj_t * base_obj, void * user_data,
    lv_event_cb_t btn_cb, int32_t col_pos, int32_t col_span,
    const char * symbol);
static void ui_loading(lv_obj_t * base_obj, uint32_t num);
static void ui_update(lv_obj_t * base_obj, uint32_t num, const char * title,
    uint8_t * bitmap, uint32_t w, uint32_t h, const char * text,
    bool is_latest, bool allow_leftright, bool allow_retry, bool allow_exit);
// Thread ---------------------------------------------------------------------
static void * thread_cb(void * arg);
static void thread_setup_webclient(ctx_t * c);
static void thread_send_result(ctx_t * c, result_t result);
static bool thread_assert(ctx_t * c, bool expr, result_t errcode);
// Thread webclient callbacks -------------------------------------------------
static int recv_raw_json_cb(char ** buffer, int offset, int datend,
    int * buflen, void * arg);
static int recv_img_cb(char ** buffer, int offset, int datend,
    int * buflen, void * arg);
// Thread png callbacks -------------------------------------------------------
static void info_cb(png_structp png_ptr, png_infop info);
static void row_cb(png_structp png_ptr, png_bytep new_row,
    png_uint_32 row_num, int pass);
static void end_cb(png_structp png_ptr, png_infop info);

/**********************
 *  STATIC VARIABLES
 **********************/

static const unsigned char xkcd_tls_root_cert[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4G\n"
    "A1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNp\n"
    "Z24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4\n"
    "MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMzETMBEG\n"
    "A1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCASIwDQYJKoZI\n"
    "hvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWtiHL8\n"
    "RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsT\n"
    "gHeMCOFJ0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmm\n"
    "KPZpO/bLyCiR5Z2KYVc3rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zd\n"
    "QQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjlOCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZ\n"
    "XriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2xmmFghcCAwEAAaNCMEAw\n"
    "DgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYEFI/wS3+o\n"
    "LkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZU\n"
    "RUm7lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMp\n"
    "jjM5RcOO5LlXbKr8EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK\n"
    "6fBdRoyV3XpYKBovHd7NADdBj+1EbddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQX\n"
    "mcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18YIvDQVETI53O9zJrlAGomecs\n"
    "Mx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7rkpeDMdmztcpH\n"
    "WD9f\n"
    "-----END CERTIFICATE-----\n"
;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void xkcd_app_run(lv_obj_t * base_obj)
{
    int res;

    ctx_t * c = malloc(sizeof(*c));
    assert(c);

    c->latest_comic_num = 0;

    res = pthread_mutex_init(&c->mutex, NULL);
    assert(res == 0);

    res = pthread_cond_init(&c->cond, NULL);
    assert(res == 0);

    create_single_byte_pipe(&c->reader, &c->writer);

    c->pollp = mcp_lvgl_poll_add(c->reader, poll_cb, EPOLLIN, c);

    c->base_obj = base_obj;
    ui_create(base_obj, c);
    ui_loading(base_obj, 0);

    pthread_attr_t attr;
    res = pthread_attr_init(&attr);
    assert(res == 0);

    res = pthread_attr_setstacksize(&attr,
        CONFIG_MCP_APPS_XKCD_NETWORKING_THREAD_STACKSIZE);
    assert(res == 0);

    c->shared_comic_num_request = COMIC_NUM_REQUEST_LATEST;
    c->waiting = true;

    res = pthread_create(&c->thread, &attr, thread_cb, c);
    assert(res == 0);

    res = pthread_attr_destroy(&attr);
    assert(res == 0);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

// Common ---------------------------------------------------------------------

static void mutex_lock(pthread_mutex_t * mutex)
{
    int res = pthread_mutex_lock(mutex);
    assert(res == 0);
}

static void mutex_unlock(pthread_mutex_t * mutex)
{
    int res = pthread_mutex_unlock(mutex);
    assert(res == 0);
}

static const char * result_string(result_t result)
{
    switch (result) {
        case RESULT_SUCCESS: return "SUCCESS";
        case RESULT_NET_ERROR: return "NET_ERROR";
        case RESULT_JSON_TOO_BIG_ERROR: return "JSON_TOO_BIG_ERROR";
        case RESULT_IMAGE_FORMAT_ERROR: return "IMAGE_FORMAT_ERROR";
        case RESULT_IMAGE_TOO_BIG_ERROR: return "IMAGE_TOO_BIG_ERROR";
        case RESULT_JSON_API_ERROR: return "JSON_API_ERROR";
        case RESULT_PNG_ERROR: return "PNG_ERROR";
        case RESULT_MEM_ALLOC_ERROR: return "MEM_ALLOC_ERROR";
        default: return "invalid result value";
    }
}

// an exercise in not depending on eventfd
static void create_single_byte_pipe(int * reader, int * writer)
{
    int res;

    int pipefd[2];
    res = pipe(pipefd);
    assert(res == 0);

    // reclaim some RAM by freeing the default size pipe buffer
    res = fcntl(pipefd[0], F_SETPIPE_SZ, 1);
    assert(res != -1);

    // we only write/read when there's space/data available.
    // this is just to assert that the assumption holds
    fd_set_nonblocking(pipefd[0]);
    fd_set_nonblocking(pipefd[1]);

    *reader = pipefd[0];
    *writer = pipefd[1];
}

static void fd_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    assert(flags != -1);
    int res = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    assert(res != -1);
}

// App ------------------------------------------------------------------------

static void poll_cb(mcp_lvgl_poll_t * handle, int fd,
    uint32_t revents, void * user_data)
{
    ctx_t * c = user_data;

    c->waiting = false;

    uint8_t pipe_val;
    ssize_t rwres = read(c->reader, &pipe_val, sizeof(pipe_val));
    assert(rwres == sizeof(pipe_val));

    mutex_lock(&c->mutex);
    result_t result = c->shared_result;
    uint32_t comic_num = c->shared_comic_num;
    char * title = c->shared_title;
    uint8_t * bitmap = c->shared_bitmap;
    uint32_t width = c->shared_width;
    uint32_t height = c->shared_height;
    mutex_unlock(&c->mutex);

    c->displayed_comic_num = comic_num;
    if(result != RESULT_SUCCESS) bitmap = NULL;

    if (comic_num > c->latest_comic_num) c->latest_comic_num = comic_num;

    const char * text = bitmap ? NULL : result_string(result);
    bool is_latest = comic_num == c->latest_comic_num;
    bool allow_leftright = true;
    bool allow_retry = result != RESULT_SUCCESS;
    bool allow_exit = true;
    ui_update(c->base_obj, comic_num, title, bitmap, width, height, text,
        is_latest, allow_leftright, allow_retry, allow_exit);
}

static void left_btn_cb(lv_event_t * e)
{
    btn_request_helper(e, -1);
}

static void right_btn_cb(lv_event_t * e)
{
    btn_request_helper(e, 1);
}

static void retry_btn_cb(lv_event_t * e)
{
    btn_request_helper(e, 0);
}

static void exit_btn_cb(lv_event_t * e)
{
    ctx_t * c = lv_event_get_user_data(e);
    int res;

    lv_obj_t * btn = lv_event_get_current_target_obj(e);
    if (lv_obj_has_state(btn, LV_STATE_DISABLED)) return;
    if (c->waiting) return; // not really expected to be reached

    send_request(c, COMIC_NUM_REQUEST_QUIT);

    mcp_lvgl_poll_remove(c->pollp);

    lv_obj_delete(c->base_obj);

    res = pthread_join(c->thread, NULL);
    assert(res == 0);

    res = pthread_mutex_destroy(&c->mutex);
    assert(res == 0);
    res = pthread_cond_destroy(&c->cond);
    assert(res == 0);

    res = close(c->reader);
    assert(res == 0);
    res = close(c->writer);
    assert(res == 0);

    free(c);
}

static void btn_request_helper(lv_event_t * e, int delta)
{
    ctx_t * c = lv_event_get_user_data(e);

    lv_obj_t * btn = lv_event_get_current_target_obj(e);
    if (lv_obj_has_state(btn, LV_STATE_DISABLED)) return;
    if (c->waiting) return; // not really expected to be reached

    int32_t num_to_request = c->displayed_comic_num ?
        c->displayed_comic_num + delta : COMIC_NUM_REQUEST_LATEST;

    ui_loading(c->base_obj, num_to_request != COMIC_NUM_REQUEST_LATEST ?
        num_to_request : 0);

    send_request(c, num_to_request);
}

static void send_request(ctx_t * c, int32_t num)
{
    mutex_lock(&c->mutex);
    c->shared_comic_num_request = num;
    int res = pthread_cond_signal(&c->cond);
    assert(res == 0);
    mutex_unlock(&c->mutex);

    c->waiting = true;
}

// App UI ---------------------------------------------------------------------

static void ui_create(lv_obj_t * base_obj, void * user_data)
{
    lv_group_add_obj(lv_group_get_default(), base_obj);
    lv_gridnav_add(base_obj, LV_GRIDNAV_CTRL_SCROLL_FIRST);

    lv_obj_set_style_pad_all(base_obj, 8, 0);
    lv_obj_set_style_pad_gap(base_obj, 2, 0);
    static const int32_t col_dsc[] = {LV_GRID_FR(1),
                                      LV_GRID_FR(1),
                                      LV_GRID_FR(1),
                                      LV_GRID_FR(1),
                                      LV_GRID_FR(1),
                                      LV_GRID_FR(1),
                                      LV_GRID_FR(1),
                                      LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t row_dsc[] = {LV_GRID_CONTENT,
                                      LV_GRID_CONTENT,
                                      LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(base_obj, col_dsc, row_dsc);

    // CHILD_TITLE
    lv_obj_t * title_label = lv_label_create(base_obj);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_grid_cell(title_label, LV_GRID_ALIGN_STRETCH, 0, 8,
        LV_GRID_ALIGN_START, 0, 1);

    // CHILD_NUM_LABEL
    lv_obj_t * num_label = lv_label_create(base_obj);
    lv_obj_set_style_text_font(num_label, &lv_font_montserrat_12, 0);
    lv_obj_set_grid_cell(num_label, LV_GRID_ALIGN_START, 0, 2,
        LV_GRID_ALIGN_END, 1, 1);

    // CHILD_LEFT_BTN
    ui_create_btn_helper(base_obj, user_data,
        left_btn_cb, 2, 2, LV_SYMBOL_LEFT);

    // CHILD_RIGHT_BTN
    ui_create_btn_helper(base_obj, user_data,
        right_btn_cb, 4, 2, LV_SYMBOL_RIGHT);

    // CHILD_RETRY_BTN
    ui_create_btn_helper(base_obj, user_data,
        retry_btn_cb, 6, 1, LV_SYMBOL_REFRESH);

    // CHILD_EXIT_BTN
    ui_create_btn_helper(base_obj, user_data,
        exit_btn_cb, 7, 1, LV_SYMBOL_CLOSE);

    // CHILD_CANVAS_BOX
    lv_obj_t * canv_cont = lv_obj_create(base_obj);
    lv_obj_set_style_bg_color(canv_cont,
        lv_palette_lighten(LV_PALETTE_BLUE, 5), LV_STATE_FOCUSED);
    lv_obj_set_grid_cell(canv_cont, LV_GRID_ALIGN_STRETCH, 0, 8,
        LV_GRID_ALIGN_STRETCH, 2, 1);
}

static void ui_create_btn_helper(lv_obj_t * base_obj, void * user_data,
    lv_event_cb_t btn_cb, int32_t col_pos, int32_t col_span,
    const char * symbol)
{
    lv_obj_t * btn = lv_button_create(base_obj);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, user_data);
    lv_group_remove_obj(btn);
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col_pos, col_span,
        LV_GRID_ALIGN_START, 1, 1);
    lv_obj_t * btn_label = lv_label_create(btn);
    lv_obj_center(btn_label);
    lv_label_set_text_static(btn_label, symbol);
}

static void ui_loading(lv_obj_t * base_obj, uint32_t num)
{
    ui_update(base_obj, num, "loading...", NULL, 0, 0, NULL,
        false, false, false, false);
}

static void ui_update(lv_obj_t * base_obj, uint32_t num, const char * title,
    uint8_t * bitmap, uint32_t w, uint32_t h, const char * text,
    bool is_latest, bool allow_leftright, bool allow_retry, bool allow_exit)
{
    lv_obj_t * title_label = lv_obj_get_child(base_obj, CHILD_TITLE);
    lv_label_set_text_static(title_label, title ? title : "(error)");

    lv_obj_t * num_label = lv_obj_get_child(base_obj, CHILD_NUM_LABEL);
    if(num) lv_label_set_text_fmt(num_label, "#%"PRIu32, num);
    lv_obj_set_flag(num_label, LV_OBJ_FLAG_HIDDEN, !(num));

    lv_obj_t * left_btn = lv_obj_get_child(base_obj, CHILD_LEFT_BTN);
    lv_obj_set_state(left_btn, LV_STATE_DISABLED,
        !(allow_leftright && num > 1));

    lv_obj_t * right_btn = lv_obj_get_child(base_obj, CHILD_RIGHT_BTN);
    lv_obj_set_state(right_btn, LV_STATE_DISABLED,
        !(allow_leftright && num && !is_latest));

    lv_obj_t * retry_btn = lv_obj_get_child(base_obj, CHILD_RETRY_BTN);
    lv_obj_set_state(retry_btn, LV_STATE_DISABLED, !(allow_retry));

    lv_obj_t * exit_btn = lv_obj_get_child(base_obj, CHILD_EXIT_BTN);
    lv_obj_set_state(exit_btn, LV_STATE_DISABLED, !(allow_exit));

    lv_obj_t * canv_box = lv_obj_get_child(base_obj, CHILD_CANVAS_BOX);
    lv_obj_clean(canv_box);
    if (bitmap) {
        lv_obj_t * canv = lv_canvas_create(canv_box);
        lv_group_remove_obj(canv);
        lv_canvas_set_buffer(canv, bitmap, w, h, LV_COLOR_FORMAT_L8);
    }
    if (text) {
        lv_obj_t * label = lv_label_create(canv_box);
        lv_group_remove_obj(label);
        lv_label_set_text_static(label, text);
    }
}

// Thread ---------------------------------------------------------------------

static void * thread_cb(void * arg)
{
    ctx_t * c = arg;
    int res;

    c->bitmap = NULL;
    c->json = NULL;
    c->pngp = NULL;
    c->infop = NULL;

    c->tls_ctx = mcp_tls_ctx_create();
    assert(c->tls_ctx);
    res = mcp_tls_ctx_add_cert(c->tls_ctx, xkcd_tls_root_cert,
        sizeof(xkcd_tls_root_cert) - 1, MCP_TLS_FLAG_PEM);
    assert(res == 0);

    while (1) {
        mutex_lock(&c->mutex);
        while (c->shared_comic_num_request == COMIC_NUM_REQUEST_NONE) {
            res = pthread_cond_wait(&c->cond, &c->mutex);
            assert(res == 0);
        }
        int32_t comic_num_request = c->shared_comic_num_request;
        mutex_unlock(&c->mutex);

        c->comic_num = comic_num_request > 0 ? comic_num_request : 0;
        c->specific_error = 0;
        c->title = NULL;
        png_destroy_read_struct(&c->pngp, &c->infop, NULL);
        cJSON_Delete(c->json); // handles NULL
        c->json = NULL;
        free(c->bitmap);
        c->bitmap = NULL;

        if (comic_num_request == COMIC_NUM_REQUEST_QUIT) {
            break;
        }

        thread_setup_webclient(c);
        if (comic_num_request == COMIC_NUM_REQUEST_LATEST) {
            c->wctx.url = "https://xkcd.com/info.0.json";
        } else {
            snprintf(c->j.urlbuf, sizeof(c->j.urlbuf),
                "https://xkcd.com/%"PRId32"/info.0.json", comic_num_request);
            c->wctx.url = c->j.urlbuf;
        }
        c->wctx.sink_callback = recv_raw_json_cb;
        c->j.raw_json_i = 0;
        res = webclient_perform(&c->wctx);
        if(thread_assert(c, res == 0,
            c->specific_error ? c->specific_error : RESULT_NET_ERROR
            )) continue;

        c->json = cJSON_ParseWithLength(
            c->j.raw_json_buf, c->j.raw_json_i);
        if(thread_assert(c, c->json, RESULT_JSON_API_ERROR)) continue;

        char * img_url = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(c->json, "img"));
        if(thread_assert(c, img_url, RESULT_JSON_API_ERROR)) continue;
        char * ext = strrchr(img_url, '.');
        if(thread_assert(c, ext && 0 == strcmp(ext, ".png"),
            RESULT_IMAGE_FORMAT_ERROR)) continue;

        c->title = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(c->json, "safe_title"));
        if(thread_assert(c, c->title, RESULT_JSON_API_ERROR)) continue;

        double comic_num_double = cJSON_GetNumberValue(
            cJSON_GetObjectItemCaseSensitive(c->json, "num"));
        if(thread_assert(c, !isnan(comic_num_double),
            RESULT_JSON_API_ERROR)) continue;

        int32_t comic_num_signed = comic_num_double;
        if(thread_assert(c, comic_num_signed > 0,
            RESULT_JSON_API_ERROR)) continue;
        c->comic_num = comic_num_signed;

        c->pngp = png_create_read_struct(PNG_LIBPNG_VER_STRING,
            NULL, NULL, NULL);
        if(thread_assert(c, c->pngp, RESULT_PNG_ERROR)) continue;
        c->infop = png_create_info_struct(c->pngp);
        if(thread_assert(c, c->infop, RESULT_PNG_ERROR)) continue;

        if (setjmp(png_jmpbuf(c->pngp))) {
            thread_send_result(c, RESULT_PNG_ERROR);
            continue;
        }

        png_set_progressive_read_fn(c->pngp, c, info_cb, row_cb, end_cb);

        thread_setup_webclient(c);
        c->wctx.url = img_url;
        c->wctx.sink_callback = recv_img_cb;
        c->p.end_reached = false;
        res = webclient_perform(&c->wctx);
        if(thread_assert(c, res == 0,
            c->specific_error ? c->specific_error : RESULT_NET_ERROR
            )) continue;
        if(thread_assert(c, c->p.end_reached, RESULT_PNG_ERROR)) continue;

        thread_send_result(c, RESULT_SUCCESS);
    }

    mcp_tls_ctx_destroy(c->tls_ctx);

    return NULL;
}

static void thread_setup_webclient(ctx_t * c)
{
    webclient_set_defaults(&c->wctx);
    c->wctx.sink_callback_arg = c;
    c->wctx.timeout_sec = 5;
    c->wctx.buffer = c->client_buf;
    c->wctx.buflen = sizeof(c->client_buf);
    c->wctx.tls_ops = &mcp_tls_webclient_tls_ops;
    c->wctx.tls_ctx = c->tls_ctx;
}

static void thread_send_result(ctx_t * c, result_t result)
{
    mutex_lock(&c->mutex);
    c->shared_comic_num_request = COMIC_NUM_REQUEST_NONE;
    c->shared_result = result;
    c->shared_comic_num = c->comic_num;
    c->shared_title = c->title;
    c->shared_bitmap = c->bitmap;
    c->shared_width = c->width;
    c->shared_height = c->height;
    mutex_unlock(&c->mutex);

    uint8_t pipe_val = 0;
    ssize_t rwres = write(c->writer, &pipe_val, sizeof(pipe_val));
    assert(rwres == sizeof(pipe_val));
}

static bool thread_assert(ctx_t * c, bool expr, result_t errcode)
{
    if (!expr) {
        thread_send_result(c, errcode);
        return true;
    }
    return false;
}

// Thread webclient callbacks -------------------------------------------------

static int recv_raw_json_cb(char ** buffer, int offset, int datend,
    int * buflen, void * arg)
{
    ctx_t * c = arg;
    char * src = *buffer + offset;
    size_t src_len = datend - offset;

    if(c->j.raw_json_i + src_len > RAW_JSON_BUFSZ) {
        c->specific_error = RESULT_JSON_TOO_BIG_ERROR;
        return -E2BIG;
    }

    memcpy(&c->j.raw_json_buf[c->j.raw_json_i], src, src_len);
    c->j.raw_json_i += src_len;

    return 0;
}

static int recv_img_cb(char ** buffer, int offset, int datend,
    int * buflen, void * arg)
{
    ctx_t * c = arg;
    char * src = *buffer + offset;
    size_t src_len = datend - offset;

    if (setjmp(png_jmpbuf(c->pngp))) {
        c->specific_error = RESULT_PNG_ERROR;
        return -EINVAL;
    }

    png_process_data(c->pngp, c->infop, (png_bytep)src, src_len);
    if (c->specific_error) return
        c->specific_error == RESULT_IMAGE_FORMAT_ERROR ? -ENOTSUP :
        c->specific_error == RESULT_IMAGE_TOO_BIG_ERROR ? -E2BIG :
        c->specific_error == RESULT_MEM_ALLOC_ERROR ? -ENOMEM :
        -EINVAL; // the specific errno doesn't really matter

    return 0;
}

// Thread png callbacks -------------------------------------------------------

static void info_cb(png_structp png_ptr, png_infop info)
{
    ctx_t * c = png_get_progressive_ptr(png_ptr);
    png_uint_32 pngres;
    int bit_depth, color_type, interlace_type;

    pngres = png_get_IHDR(png_ptr, info, &c->width, &c->height,
        &bit_depth, &color_type, &interlace_type, NULL, NULL);
    assert(pngres == 1);

    if(
        interlace_type != PNG_INTERLACE_NONE
        || (color_type != PNG_COLOR_TYPE_GRAY && color_type != PNG_COLOR_TYPE_RGB)
        || bit_depth != 8
    ) {
        c->specific_error = RESULT_IMAGE_FORMAT_ERROR;
        (void)png_process_data_pause(png_ptr, 0);
        return;
    }

    if (color_type == PNG_COLOR_TYPE_RGB) {
        png_set_rgb_to_gray(png_ptr, PNG_ERROR_ACTION_NONE, -1.0, -1.0);
    }

    size_t size_bytes = c->width * c->height;

    if(
        size_bytes > MAX_DECODED_IMG_BYTES
    ) {
        c->specific_error = RESULT_IMAGE_TOO_BIG_ERROR;
        (void)png_process_data_pause(png_ptr, 0);
        return;
    }

    c->bitmap = malloc(size_bytes);

    if(c->bitmap == NULL) {
        c->specific_error = RESULT_MEM_ALLOC_ERROR;
        (void)png_process_data_pause(png_ptr, 0);
        return;
    }

    png_start_read_image(png_ptr);
}

static void row_cb(png_structp png_ptr, png_bytep new_row,
    png_uint_32 row_num, int pass)
{
    ctx_t * c = png_get_progressive_ptr(png_ptr);

    memcpy(&c->bitmap[c->width * row_num], new_row, c->width);
}

static void end_cb(png_structp png_ptr, png_infop info)
{
    ctx_t * c = png_get_progressive_ptr(png_ptr);

    c->p.end_reached = true;
}
