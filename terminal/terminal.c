#include <mcp/terminal.h>
#include <mcp/mcp_lvgl_common.h>

#include <stdlib.h>
#include <assert.h>
#include <pty.h>
#include <spawn.h>
#include <signal.h>
#include <errno.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "libtmt/tmt.h"

#define CELL_W 8
#define CELL_H 9
#define FONT lv_font_unscii_8
#define FOREGROUND 0xf8f8f2
#define BACKGROUND 0x282a36
#define MASTER_READ_BUF_SIZE 1024

typedef struct {
    lv_obj_t * base_obj;
    lv_obj_t * term_obj;
    uint32_t n_cells_x;
    uint32_t n_cells_y;
    TMT * term;
    bool exited;
    uint32_t click_event_debt;
    uint32_t cursor_x;
    uint32_t cursor_y;
    int master;
    int slave;
    pid_t pid;
    int sfd;
    mcp_lvgl_poll_t * master_poll_hdl;
    mcp_lvgl_poll_t * signal_poll_hdl;
    sigset_t oldset;
    char master_read_buf[MASTER_READ_BUF_SIZE];
    char * cell_chars;
} ctx_t;

static void cell_area(
    lv_area_t * dst,
    int32_t offset_x,
    int32_t offset_y,
    int32_t cell_x1,
    int32_t cell_y1,
    int32_t cell_x2,
    int32_t cell_y2
)
{
    dst->x1 = offset_x + cell_x1 * CELL_W;
    dst->y1 = offset_y + cell_y1 * CELL_H;
    dst->x2 = offset_x + cell_x2 * CELL_W + (CELL_W - 1);
    dst->y2 = offset_y + cell_y2 * CELL_H + (CELL_H - 1);
}

static void key_handler(ctx_t * ctx, uint32_t key)
{
    ssize_t rwres;

    char seq[3];
    size_t seq_len;

    switch(key) {
        case ' ' ... '~':
            seq[0] = key;
            seq_len = 1;
            break;
        case LV_KEY_ESC:
            seq[0] = 27; /* ASCII ESC (escape) */
            seq_len = 1;
            break;
        case LV_KEY_ENTER:
            seq[0] = '\n';
            seq_len = 1;
            break;
        case LV_KEY_BACKSPACE:
            seq[0] = 127; /* ASCII DEL */
            seq_len = 1;
            break;
        case LV_KEY_UP:
            seq[0] = 27;
            seq[1] = '[';
            seq[2] = 'A';
            seq_len = 3;
            break;
        case LV_KEY_DOWN:
            seq[0] = 27;
            seq[1] = '[';
            seq[2] = 'B';
            seq_len = 3;
            break;
        case LV_KEY_RIGHT:
            seq[0] = 27;
            seq[1] = '[';
            seq[2] = 'C';
            seq_len = 3;
            break;
        case LV_KEY_LEFT:
            seq[0] = 27;
            seq[1] = '[';
            seq[2] = 'D';
            seq_len = 3;
            break;
        default:
            return;
    }

    rwres = write(ctx->master, seq, seq_len);
    if(rwres < 0) {
        assert(errno == EPIPE);
    }
    else {
        assert(rwres == seq_len);
    }
}

static void key_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);
    uint32_t key = *(uint32_t *) lv_event_get_param(e);

    if(key == LV_KEY_ENTER) {
        ctx->click_event_debt++;
    }

    key_handler(ctx, key);
}

static void click_cb(lv_event_t * e)
{
    lv_indev_t * indev = lv_event_get_indev(e);

    if(lv_indev_get_type(indev) != LV_INDEV_TYPE_KEYPAD) {
        return;
    }

    ctx_t * ctx = lv_event_get_user_data(e);

    assert(ctx->click_event_debt > 0);
    ctx->click_event_debt--;

    if(ctx->click_event_debt == 0 && ctx->exited) {
        lv_obj_delete(ctx->base_obj);
    }
}

static void draw_end_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);
    lv_layer_t * layer = lv_event_get_layer(e);
    lv_obj_t * term_obj = lv_event_get_target_obj(e);

    lv_area_t term_obj_coords;
    lv_obj_get_coords(term_obj, &term_obj_coords);

    lv_draw_letter_dsc_t dsc;
    lv_draw_letter_dsc_init(&dsc);
    dsc.font = &FONT;
    dsc.color = lv_color_hex(FOREGROUND);

    uint32_t n_cells_x = ctx->n_cells_x;
    uint32_t n_cells_y = ctx->n_cells_y;

    int32_t first_col = (layer->_clip_area.x1 - term_obj_coords.x1) / CELL_W;
    int32_t last_col = (layer->_clip_area.x2 - term_obj_coords.x1) / CELL_W;
    int32_t first_row = (layer->_clip_area.y1 - term_obj_coords.y1) / CELL_H;
    int32_t last_row = (layer->_clip_area.y2 - term_obj_coords.y1) / CELL_H;
    first_col = LV_CLAMP(0, first_col, n_cells_x - 1);
    last_col = LV_CLAMP(0, last_col, n_cells_x - 1);
    first_row = LV_CLAMP(0, first_row, n_cells_y - 1);
    last_row = LV_CLAMP(0, last_row, n_cells_y - 1);

    for(uint32_t y = first_row; y <= last_row; y++) {
        for(uint32_t x = first_col; x <= last_col; x++) {
            char c = ctx->cell_chars[y * n_cells_x + x];

            bool is_cursor = x == ctx->cursor_x && y == ctx->cursor_y;

            lv_draw_fill_dsc_t fill_dsc;
            if(is_cursor) {
                lv_draw_fill_dsc_init(&fill_dsc);
                fill_dsc.color = dsc.color;
                dsc.color = lv_color_hex(BACKGROUND);
                lv_area_t area;
                cell_area(&area, term_obj_coords.x1, term_obj_coords.y1, x, y, x, y);
                lv_draw_fill(layer, &fill_dsc, &area);
            }

            if(c) {
                dsc.unicode = c;
                lv_point_t point = {
                    .x = term_obj_coords.x1 + x * CELL_W + CELL_W / 2,
                    .y = term_obj_coords.y1 + y * CELL_H + (CELL_H - 1)
                };
                lv_draw_letter(layer, &dsc, &point);
            }

            if(is_cursor) {
                dsc.color = fill_dsc.color;
            }
        }
    }
}

static void term_cb(tmt_msg_t m, struct TMT *v, const void *r, void *p)
{
    ctx_t * ctx = p; /* don't use ctx->term because it may not be assigned yet */

    switch(m) {
        case TMT_MSG_MOVED: {
            const TMTPOINT * point = r;
            int32_t term_x = lv_obj_get_x(ctx->term_obj);
            int32_t term_y = lv_obj_get_y(ctx->term_obj);

            lv_area_t area;

            cell_area(&area, term_x, term_y, ctx->cursor_x, ctx->cursor_y, ctx->cursor_x, ctx->cursor_y);
            lv_obj_invalidate_area(ctx->term_obj, &area);

            cell_area(&area, term_x, term_y, point->c, point->r, point->c, point->r);
            lv_obj_invalidate_area(ctx->term_obj, &area);

            ctx->cursor_x = point->c;
            ctx->cursor_y = point->r;

            break;
        }

        case TMT_MSG_UPDATE: {
            const TMTSCREEN * s = r;
            int32_t term_x = lv_obj_get_x(ctx->term_obj);
            int32_t term_y = lv_obj_get_y(ctx->term_obj);

            int32_t row_min = -1;

            int32_t col_min = -1;
            int32_t col_max = 0;

            for (int32_t y = 0; y <= s->nline; y++){
                bool row_changed = false;

                if (y < s->nline && s->lines[y]->dirty){
                    for (int32_t x = 0; x < s->ncol; x++){
                        TMTCHAR * tmtchar = &s->lines[y]->chars[x];
                        char c;
                        if(
                            tmtchar->a.invisible == false
                            && tmtchar->c > ' '
                            && tmtchar->c <= '~'
                        ) c = tmtchar->c;
                        else c = '\0';

                        char * cellchar = &ctx->cell_chars[y * ctx->n_cells_x + x];

                        if(c != *cellchar) {
                            *cellchar = c;
                            row_changed = true;
                            if(col_min < 0) col_min = x;
                            col_min = col_min < 0 ? x : LV_MIN(col_min, x);
                            col_max = LV_MAX(col_max, x);
                        }
                    }
                }

                if(row_changed) {
                    if(row_min < 0) row_min = y;
                }
                else {
                    if(row_min >= 0) {
                        if(col_min >= 0) {
                            int32_t row_max = y - 1;
                            lv_area_t area;
                            cell_area(&area, term_x, term_y, col_min, row_min, col_max, row_max);
                            lv_obj_invalidate_area(ctx->term_obj, &area);

                            col_min = -1;
                            col_max = 0;
                        }

                        row_min = -1;
                    }
                }
            }

            tmt_clean(v); /* just sets `dirty` to false. `dirty` is write-only in tmt */

            break;
        }

        case TMT_MSG_ANSWER:
            break;

        case TMT_MSG_BELL:
            break;

        case TMT_MSG_CURSOR:
            break;

        default:
            break;
    }
}

void master_poll_cb(mcp_lvgl_poll_t * handle, int fd, uint32_t revents, void * user_data)
{
    ssize_t rwres;

    ctx_t * ctx = user_data;

    if(revents != EPOLLIN) {
        mcp_lvgl_poll_remove(ctx->master_poll_hdl);
        ctx->master_poll_hdl = NULL;
        return;
    }

    rwres = read(fd, ctx->master_read_buf, MASTER_READ_BUF_SIZE);
    assert(rwres > 0);

    tmt_write(ctx->term, ctx->master_read_buf, rwres);
}

void signal_poll_cb(mcp_lvgl_poll_t * handle, int fd, uint32_t revents, void * user_data)
{
    int res;

    ctx_t * ctx = user_data;

    assert(revents == EPOLLIN);

    if(ctx->exited) return;

    sigset_t set;
    res = sigemptyset(&set);
    assert(res == 0);
    res = sigaddset(&set, SIGCHLD);
    assert(res == 0);
    struct timespec zero_ts = {.tv_sec = 0, .tv_nsec = 0};
    sigtimedwait(&set, NULL, &zero_ts);

    pid_t wres = waitpid(ctx->pid, NULL, WNOHANG);

    if(wres == ctx->pid) {
        if(ctx->click_event_debt == 0) {
            lv_obj_delete(ctx->base_obj);
        }
        else {
            /*  workaround to prevent a click event from being sent
                to the app launcher after the base object is deleted,
                which could re-enter the terminal immediately. */
            ctx->exited = true;
        }
    }
    else if(wres == 0) {
    }
    else assert(0);
}

static void keyboard_event_cb(lv_event_t * e)
{
    lv_keyboard_def_event_cb(e);

    lv_obj_t * kb = lv_event_get_current_target(e);
    ctx_t * ctx = lv_event_get_user_data(e);

    uint32_t btn_id = lv_buttonmatrix_get_selected_button(kb);
    if(btn_id == LV_BUTTONMATRIX_BUTTON_NONE) return;

    const char * txt = lv_buttonmatrix_get_button_text(kb, btn_id);
    if(txt == NULL) return;

    if(txt[0] && !txt[1] && ((txt[0] >= ' ' && txt[0] <= '~'))) {
        key_handler(ctx, txt[0]);
    }
    else if(0 == strcmp(txt, LV_SYMBOL_NEW_LINE)) {
        key_handler(ctx, LV_KEY_ENTER);
    }
    else if(0 == strcmp(txt, LV_SYMBOL_BACKSPACE)) {
        key_handler(ctx, LV_KEY_BACKSPACE);
    }
    else if(0 == strcmp(txt, LV_SYMBOL_OK)) {
        key_handler(ctx, LV_KEY_ESC);
    }
    else if(0 == strcmp(txt, LV_SYMBOL_KEYBOARD) || 0 == strcmp(txt, LV_SYMBOL_CLOSE)) {
        lv_obj_delete(kb);

        int32_t w = lv_obj_get_width(ctx->term_obj);
        int32_t h = lv_obj_get_height(ctx->term_obj);
        uint32_t new_n_cells_y = h / CELL_H;

        ctx->cell_chars = realloc(ctx->cell_chars, ctx->n_cells_x * new_n_cells_y);
        assert(ctx->cell_chars);
        memset(ctx->cell_chars + ctx->n_cells_x * ctx->n_cells_y, 0, ctx->n_cells_x * (new_n_cells_y - ctx->n_cells_y));

        struct winsize win = {
            .ws_row = new_n_cells_y,
            .ws_col = ctx->n_cells_x,
            .ws_xpixel = w,
            .ws_ypixel = h,
        };
        ioctl(ctx->slave, TIOCSWINSZ, &win);

        tmt_resize(ctx->term, new_n_cells_y, ctx->n_cells_x);

        ctx->n_cells_y = new_n_cells_y;
    }
}

static void base_obj_delete_cb(lv_event_t * e)
{
    int res;

    ctx_t * ctx = lv_event_get_user_data(e);

    if(ctx->term) tmt_close(ctx->term);

    if(ctx->master_poll_hdl) mcp_lvgl_poll_remove(ctx->master_poll_hdl);
    mcp_lvgl_poll_remove(ctx->signal_poll_hdl);

    res = close(ctx->master);
    assert(res == 0);
    res = close(ctx->slave);
    assert(res == 0);

    res = close(ctx->sfd);
    assert(res == 0);

    res = sigprocmask(SIG_SETMASK, &ctx->oldset, NULL);
    assert(res == 0);

    free(ctx->cell_chars);

    free(ctx);
}

void terminal_app_run(lv_obj_t * base_obj)
{
    int res;

    lv_obj_t * term_obj = base_obj;

    lv_obj_t * kb = NULL;
    lv_indev_t * indev = NULL;
    while((indev = lv_indev_get_next(indev))) {
        if(lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            kb = lv_keyboard_create(base_obj);
            break;
        }
    }

    lv_obj_refr_size(term_obj);
    int32_t w = lv_obj_get_width(term_obj);
    int32_t h = lv_obj_get_height(term_obj);
    if(kb) {
        lv_obj_refr_size(kb);
        h -= lv_obj_get_height(kb);
    }
    uint32_t n_cells_x = w / CELL_W;
    uint32_t n_cells_y = h / CELL_H;

    ctx_t * ctx = calloc(1, sizeof(*ctx));
    assert(ctx);

    ctx->base_obj = base_obj;
    ctx->term_obj = term_obj;
    ctx->n_cells_x = n_cells_x;
    ctx->n_cells_y = n_cells_y;
    ctx->cell_chars = calloc(n_cells_x * n_cells_y, 1);
    assert(ctx->cell_chars);

    lv_obj_set_style_bg_color(term_obj, lv_color_hex(BACKGROUND), 0);

    lv_obj_add_event_cb(term_obj, key_cb, LV_EVENT_KEY, ctx);
    if(kb) {
        lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
        lv_obj_add_event_cb(kb, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, ctx);
    }
    lv_obj_add_flag(term_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(term_obj, click_cb, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(term_obj, draw_end_cb, LV_EVENT_DRAW_MAIN_END, ctx);

    lv_group_add_obj(lv_group_get_default(), term_obj);
    lv_group_focus_obj(term_obj);

    struct winsize win = {
        .ws_row = n_cells_y,
        .ws_col = n_cells_x,
        .ws_xpixel = w,
        .ws_ypixel = h,
    };

    res = openpty(&ctx->master, &ctx->slave, NULL, NULL, &win);
    assert(res == 0);

    posix_spawn_file_actions_t file_actions;
    res = posix_spawn_file_actions_init(&file_actions);
    assert(res == 0);
    res = posix_spawn_file_actions_adddup2(&file_actions, ctx->slave, 0);
    assert(res == 0);
    res = posix_spawn_file_actions_adddup2(&file_actions, ctx->slave, 1);
    assert(res == 0);
    res = posix_spawn_file_actions_adddup2(&file_actions, ctx->slave, 2);
    assert(res == 0);

    res = posix_spawn(&ctx->pid, "sh", &file_actions, NULL, NULL, NULL);
    assert(res == 0);

    res = posix_spawn_file_actions_destroy(&file_actions);
    assert(res == 0);

    ctx->master_poll_hdl = mcp_lvgl_poll_add(ctx->master, master_poll_cb, EPOLLIN, ctx);

    sigset_t set;
    res = sigemptyset(&set);
    assert(res == 0);
    res = sigaddset(&set, SIGCHLD);
    assert(res == 0);
    res = sigprocmask(SIG_BLOCK, &set, &ctx->oldset);
    assert(res == 0);
    ctx->sfd = signalfd(-1, &set, SFD_CLOEXEC);
    assert(ctx->sfd >= 0);

    ctx->signal_poll_hdl = mcp_lvgl_poll_add(ctx->sfd, signal_poll_cb, EPOLLIN, ctx);

    lv_obj_add_event_cb(base_obj, base_obj_delete_cb, LV_EVENT_DELETE, ctx);

    ctx->term = tmt_open(n_cells_y, n_cells_x, term_cb, ctx, NULL);
    assert(ctx->term);
}
