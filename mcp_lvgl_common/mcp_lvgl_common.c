#include <mcp/mcp_lvgl_common_private.h>

#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

#include <lvgl/lvgl.h> /* LV_NO_TIMER_READY, lv_timer_handler */

struct mcp_lvgl_poll_t {
    mcp_lvgl_poll_cb_t cb;
    int fd;
    uint32_t events;
    void * user_data;
    mcp_lvgl_poll_t * prev;
    mcp_lvgl_poll_t * next;
};

typedef struct {
    int ep_fd;
    struct epoll_event * ep_eventlist;
    int ep_eventlist_capacity;
    int n_polls;
    int running_poll_event_count;
    int running_poll_i;
    mcp_lvgl_poll_t * head;
} ctx_t;

static ctx_t * g;

void mcp_lvgl_poll_init(void)
{
    g = calloc(1, sizeof(*g));
    assert(g);
    g->ep_fd = epoll_create1(EPOLL_CLOEXEC);
    assert(g->ep_fd >= 0);
}

bool mcp_lvgl_poll_run(uint32_t timeout)
{
    bool ret = true;

    if(g->n_polls) {
        if(g->n_polls > g->ep_eventlist_capacity) {
            g->ep_eventlist_capacity = g->n_polls;
            free(g->ep_eventlist);
            g->ep_eventlist = malloc(g->n_polls * sizeof(*g->ep_eventlist));
            assert(g->ep_eventlist);
        }

        int epoll_timeout = timeout == LV_NO_TIMER_READY ? -1 : timeout;
        int polls_returned = epoll_wait(g->ep_fd, g->ep_eventlist, g->n_polls, epoll_timeout);
        assert(polls_returned >= 0);
        g->running_poll_event_count = polls_returned;

        for(int i = 0; i < polls_returned; i++) {
            struct epoll_event * ev = &g->ep_eventlist[i];
            mcp_lvgl_poll_t * handle = ev->data.ptr;
            if(handle == NULL) {
                continue;
            }
            uint32_t anded_events = (handle->events | (EPOLLHUP | EPOLLERR)) & ev->events;
            if(!anded_events) {
                continue;
            }
            g->running_poll_i = i;
            handle->cb(handle->fd, anded_events, handle->user_data);
        }
        g->running_poll_event_count = 0;
    }
    else if(timeout != LV_NO_TIMER_READY) {
        struct timespec ts;
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000l) * 1000000l;
        int res = nanosleep(&ts, NULL);
        assert(res == 0);
    }
    else {
        ret = false;
    }

    if(!g->n_polls && g->ep_eventlist_capacity) {
        free(g->ep_eventlist);
        g->ep_eventlist = NULL;
        g->ep_eventlist_capacity = 0;
    }

    return ret;
}

void mcp_lvgl_poll_run_until_done(void)
{
    while(mcp_lvgl_poll_run(lv_timer_handler()));
}

void mcp_lvgl_poll_deinit(void)
{
    int res = close(g->ep_fd);
    assert(res == 0);

    free(g->ep_eventlist);

    while(g->head) {
        mcp_lvgl_poll_t * node = g->head;
        g->head = node->next;
        free(node);
    }

    free(g);
}

mcp_lvgl_poll_t * mcp_lvgl_poll_add(int fd, mcp_lvgl_poll_cb_t cb, uint32_t events, void * user_data)
{
    mcp_lvgl_poll_t * handle = malloc(sizeof(*handle));
    assert(handle);
    handle->cb = cb;
    handle->fd = fd;
    handle->events = events;
    handle->user_data = user_data;

    handle->prev = NULL;
    mcp_lvgl_poll_t * old_head = g->head;
    g->head = handle;
    handle->next = old_head;
    if(old_head) old_head->prev = handle;

    struct epoll_event ep_event;
    ep_event.events = events;
    ep_event.data.ptr = handle;

    int res = epoll_ctl(g->ep_fd, EPOLL_CTL_ADD, fd, &ep_event);
    assert(res == 0);

    g->n_polls ++;

    return handle;
}

void mcp_lvgl_poll_modify(mcp_lvgl_poll_t * handle, uint32_t events)
{
    handle->events = events;

    struct epoll_event ep_event;
    ep_event.events = events;
    ep_event.data.ptr = handle;

    int res = epoll_ctl(g->ep_fd, EPOLL_CTL_MOD, handle->fd, &ep_event);
    assert(res == 0);
}

void mcp_lvgl_poll_remove(mcp_lvgl_poll_t * handle)
{
    int res = epoll_ctl(g->ep_fd, EPOLL_CTL_DEL, handle->fd, NULL);
    assert(res == 0);

    for(int i = g->running_poll_i + 1; i < g->running_poll_event_count; i++) {
        if(g->ep_eventlist[i].data.ptr == handle) {
            g->ep_eventlist[i].data.ptr = NULL;
            break;
        }
    }

    if(g->head == handle) {
        g->head = handle->next;
    }
    if(handle->prev) {
        handle->prev->next = handle->next;
    }
    if(handle->next) {
        handle->next->prev = handle->prev;
    }

    free(handle);

    g->n_polls --;
}
