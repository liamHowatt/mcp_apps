#include "mcp_board/mcp_bitbang/mcp_bitbang_client.h"
#include "mcp_board/mcp_modnet/mcp_modnet_server.h"

#include <nuttx/ioexpander/gpio.h>
#include <nuttx/timers/timer.h>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <errno.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <string.h>

#include "mcp_daemon_private.h"

#define POLLFDS_PEER_START 2

#define IS_READING 1
#define IS_WRITING 2

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ABS(x) ((x) < 0 ? -(x) : (x))

typedef struct {
    uint8_t token;

    uint8_t is_doing;
    bool was_unblocked;
    uint32_t transaction_remaining_len;
} peer_data_t;

typedef struct pin_socket_ctx_t pin_socket_ctx_t;

typedef bool (*sm_next_byte_cb_t)(pin_socket_ctx_t *);

struct pin_socket_ctx_t {
    mbb_cli_t mbb;
    sigset_t set;
    int clk_fd;
    int dat_fd;
    int tim_fd;
    int signum;
    bool clk_val;
    bool dat_val;

    uint8_t bit_state;
    sm_next_byte_cb_t next_byte_cb;
};

typedef struct {
    pin_socket_ctx_t pin_soc;
    uint8_t * buf;
    uint32_t len;
    bool is_read;
} master_socket_sm_t;

typedef struct {
    pin_socket_ctx_t pin_soc;
    uint8_t byte_state;
    uint8_t flags;

    bool something_happened;
    uint8_t new_global_token_count;
    peer_data_t * peer_datas;
    uint8_t peer_count;
} poller_socket_sm_t;

typedef struct {
    master_socket_sm_t s0;
    poller_socket_sm_t s1;
    int sfd;
} socket_sms_t;

static void pin_set(pin_socket_ctx_t * ctx, mbb_cli_pin_t pinno, bool val) {
    int res;

    bool * cur_val = pinno == MBB_CLI_PIN_CLK ? &ctx->clk_val : &ctx->dat_val;
    if(val == *cur_val) return;
    int fd = pinno == MBB_CLI_PIN_CLK ? ctx->clk_fd : ctx->dat_fd;
    res = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long) (val ? GPIO_INTERRUPT_RISING_PIN : GPIO_OUTPUT_PIN));
    assert(res >= 0);
    *cur_val = val;
}

static bool pin_get(pin_socket_ctx_t * ctx, mbb_cli_pin_t pinno)
{
    int res;

    bool output_val = pinno == MBB_CLI_PIN_CLK ? ctx->clk_val : ctx->dat_val;
    if(!output_val) {
        return false;
    }
    int fd = pinno == MBB_CLI_PIN_CLK ? ctx->clk_fd : ctx->dat_fd;
    bool val;
    res = ioctl(fd, GPIOC_READ, (unsigned long)((uintptr_t)&val));
    assert(res >= 0);
    return val;
}

static void pin_set_cb(void * vctx, mbb_cli_pin_t pinno, bool val)
{
    pin_set((pin_socket_ctx_t *)vctx, pinno, val);
}

static bool pin_get_cb(void * vctx, mbb_cli_pin_t pinno)
{
    return pin_get((pin_socket_ctx_t *)vctx, pinno);
}

static void pin_wait(pin_socket_ctx_t * ctx, mbb_cli_pin_t pinno)
{
    int res;

    bool output_val = pinno == MBB_CLI_PIN_CLK ? ctx->clk_val : ctx->dat_val;
    assert(output_val);
    int fd = pinno == MBB_CLI_PIN_CLK ? ctx->clk_fd : ctx->dat_fd;

    struct sigevent notify;
    notify.sigev_notify = SIGEV_SIGNAL;
    notify.sigev_signo  = ctx->signum;

    res = ioctl(fd, GPIOC_REGISTER, (unsigned long)&notify);
    assert(res >= 0);

    if(!pin_get(ctx, pinno)) {
        res = sigwaitinfo(&ctx->set, NULL);
        assert(res >= 0);
    }

    res = ioctl(fd, GPIOC_UNREGISTER, 0);
    assert(res >= 0);

    const struct timespec zero_ts = {.tv_sec = 0, .tv_nsec = 0};
    res = sigtimedwait(&ctx->set, NULL, &zero_ts);
    assert(res >= 0 || errno == EAGAIN);
}

static void timer_sleep(pin_socket_ctx_t * ctx)
{
    int res;

    struct timer_notify_s notify;
    notify.pid      = getpid();
    notify.periodic = false;
    notify.event.sigev_notify = SIGEV_SIGNAL;
    notify.event.sigev_signo  = ctx->signum;
    notify.event.sigev_value.sival_ptr = NULL;
    res = ioctl(ctx->tim_fd, TCIOC_NOTIFICATION, (unsigned long)((uintptr_t)&notify));
    assert(res == 0);

    res = ioctl(ctx->tim_fd, TCIOC_START, 0);
    assert(res == 0);
    res = sigwaitinfo(&ctx->set, NULL);
    assert(res >= 0);
}

static void run_transfer(pin_socket_ctx_t * ctx)
{
    mbb_cli_status_t status;
    while (MBB_CLI_STATUS_DONE != (status = mbb_cli_continue_byte_transfer(&ctx->mbb))) {
        switch (status) {
            case MBB_CLI_STATUS_DO_DELAY:
                timer_sleep(ctx);
                break;
            case MBB_CLI_STATUS_WAIT_CLK_PIN_HIGH:
                pin_wait(ctx, MBB_CLI_PIN_CLK);
                break;
            default:
                assert(0);
        }
    }
}

static uint8_t do_read(pin_socket_ctx_t * ctx)
{
    mbb_cli_start_byte_transfer(&ctx->mbb, MBB_CLI_BYTE_TRANSFER_READ);
    run_transfer(ctx);
    return mbb_cli_get_read_byte(&ctx->mbb);
}

static void do_write(pin_socket_ctx_t * ctx, uint8_t data)
{
    mbb_cli_start_byte_transfer(&ctx->mbb, MBB_CLI_BYTE_TRANSFER_WRITE(data));
    run_transfer(ctx);
}

static void pin_socket_ctx_init(pin_socket_ctx_t * ctx,
                                const char * clk_path, const char * dat_path,
                                const char * tim_path, int signum,
                                sm_next_byte_cb_t next_byte_cb)
{
    int res;

    ctx->clk_fd = open(clk_path, O_RDWR | O_CLOEXEC);
    assert(ctx->clk_fd >= 0);
    ctx->dat_fd = open(dat_path, O_RDWR | O_CLOEXEC);
    assert(ctx->dat_fd >= 0);

    ctx->tim_fd = open(tim_path, O_RDONLY | O_CLOEXEC);
    assert(ctx->tim_fd >= 0);
    res = ioctl(ctx->tim_fd, TCIOC_SETTIMEOUT, 500); /* 500 us */
    assert(res == 0);

    res = sigemptyset(&ctx->set);
    assert(res == 0);
    res = sigaddset(&ctx->set, signum);
    assert(res == 0);
    res = sigprocmask(SIG_BLOCK, &ctx->set, NULL);
    assert(res == 0);

    ctx->signum = signum;

    ctx->clk_val = false;
    ctx->dat_val = false;

    ctx->next_byte_cb = next_byte_cb;
    ctx->bit_state = MBB_CLI_STATUS_DONE;

    mbb_cli_init(&ctx->mbb, pin_get_cb, pin_set_cb, ctx);

    pin_set(ctx, MBB_CLI_PIN_CLK, 1);
    pin_set(ctx, MBB_CLI_PIN_DAT, 1);

    pin_wait(ctx, MBB_CLI_PIN_CLK);
    pin_wait(ctx, MBB_CLI_PIN_DAT);

    timer_sleep(ctx);
    pin_set(ctx, MBB_CLI_PIN_CLK, 0);
    timer_sleep(ctx);
}

// static void pin_socket_ctx_deinit(pin_socket_ctx_t * ctx)
// {
//     close(ctx->clk_fd);
//     close(ctx->dat_fd);
//     close(ctx->tim_fd);
// }

static void run_socket(pin_socket_ctx_t * ctx, uint8_t token)
{
    do_write(ctx, MMN_SRV_OPCODE_WHEREAMI);
    uint8_t where = do_read(ctx);
    printf("where: %d\n", (int) where);

    do_write(ctx, MMN_SRV_OPCODE_WRITE);
    do_write(ctx, 4); // "cpu4" is 4 bytes
    do_write(ctx, token); // send to self
    uint8_t free_space = do_read(ctx);
    printf("free space: %d\n", (int) free_space);
    assert(free_space >= 4);
    do_write(ctx, 'c');
    do_write(ctx, 'p');
    do_write(ctx, 'u');
    do_write(ctx, '4');

    do_write(ctx, MMN_SRV_OPCODE_READ);
    do_write(ctx, 4); // "cpu4" is 4 bytes
    do_write(ctx, token); // recv from self
    uint8_t readable = do_read(ctx);
    printf("amount readable: %d\n", (int) readable);
    assert(readable == 4);
    assert(do_read(ctx) == 'c');
    assert(do_read(ctx) == 'p');
    assert(do_read(ctx) == 'u');
    assert(do_read(ctx) == '4');
    printf("'cpu4' received\n");
}

static void poller_sm_start_over(poller_socket_sm_t * sm)
{
    mbb_cli_start_byte_transfer(&sm->pin_soc.mbb, MBB_CLI_BYTE_TRANSFER_WRITE(MMN_SRV_OPCODE_POLL));
    sm->byte_state = 0;
}

static bool poller_sm_next_byte_cb(pin_socket_ctx_t * pin_soc)
{
    poller_socket_sm_t * sm = (poller_socket_sm_t *) pin_soc;

    switch(sm->byte_state) {
        case 0: /* poll opcode was sent. send timeout */
            /* infinite timeout */
            mbb_cli_start_byte_transfer(&sm->pin_soc.mbb, MBB_CLI_BYTE_TRANSFER_WRITE(255));
            sm->byte_state++;
            break;
        case 1: /* timout was sent. read the flags */
            mbb_cli_start_byte_transfer(&sm->pin_soc.mbb, MBB_CLI_BYTE_TRANSFER_READ);
            sm->byte_state++;
            break;
        case 2: /* got the flags. decide what to read next */
            sm->flags = mbb_cli_get_read_byte(&sm->pin_soc.mbb);
            if(sm->flags & MMN_SRV_FLAG_PRESENCE) {
                sm->byte_state += 1;
            }
            else if (sm->flags & (MMN_SRV_FLAG_READABLE | MMN_SRV_FLAG_WRITABLE)) {
                sm->byte_state += 2;
            }
            else assert(0); /* empty flags not possible with inf timeout */
            mbb_cli_start_byte_transfer(&sm->pin_soc.mbb, MBB_CLI_BYTE_TRANSFER_READ);
            break;
        case 3: /* got the new token count */
            sm->new_global_token_count = mbb_cli_get_read_byte(&sm->pin_soc.mbb);
            sm->something_happened = true;
            if (sm->flags & (MMN_SRV_FLAG_READABLE | MMN_SRV_FLAG_WRITABLE)) {
                mbb_cli_start_byte_transfer(&sm->pin_soc.mbb, MBB_CLI_BYTE_TRANSFER_READ);
                sm->byte_state++;
            }
            else {
                poller_sm_start_over(sm);
            }
            break;
        case 4: { /* got the token with a readable/writable status */
            uint8_t readable_and_or_writable_token = mbb_cli_get_read_byte(&sm->pin_soc.mbb);
            peer_data_t * pd = sm->peer_datas;
            peer_data_t * pd_end = pd + sm->peer_count;
            for( ; pd != pd_end; pd++) if(pd->token == readable_and_or_writable_token) break;
            if(pd == pd_end) break;
            if((pd->is_doing == IS_READING && (sm->flags & MMN_SRV_FLAG_READABLE))
               || (pd->is_doing == IS_WRITING && (sm->flags & MMN_SRV_FLAG_WRITABLE))) {
                sm->something_happened = true;
                pd->was_unblocked = true;
            }
            poller_sm_start_over(sm);
            break;
        }
    }

    return false;
}

static bool sm_next(pin_socket_ctx_t * sm)
{
    int res;
    const struct timespec zero_ts = {.tv_sec = 0, .tv_nsec = 0};

    if(sm->bit_state != MBB_CLI_STATUS_DONE) {
        if (sm->bit_state == MBB_CLI_STATUS_WAIT_CLK_PIN_HIGH) {
            res = ioctl(sm->clk_fd, GPIOC_UNREGISTER, 0);
            assert(res >= 0);
        }
        else assert(sm->bit_state == MBB_CLI_STATUS_DO_DELAY);

        res = sigtimedwait(&sm->set, NULL, &zero_ts);
        assert(res >= 0);
    }

    while(1) {
        sm->bit_state = mbb_cli_continue_byte_transfer(&sm->mbb);
        if(sm->bit_state == MBB_CLI_STATUS_DONE) {
            bool done = sm->next_byte_cb(sm);
            if(done) return true;
        }
        else if(sm->bit_state == MBB_CLI_STATUS_WAIT_CLK_PIN_HIGH) {
            assert(sm->clk_val);

            struct sigevent notify;
            notify.sigev_notify = SIGEV_SIGNAL;
            notify.sigev_signo  = sm->signum;

            res = ioctl(sm->clk_fd, GPIOC_REGISTER, (unsigned long)&notify);
            assert(res >= 0);

            if(pin_get(sm, MBB_CLI_PIN_CLK)) {
                res = ioctl(sm->clk_fd, GPIOC_UNREGISTER, 0);
                assert(res >= 0);

                res = sigtimedwait(&sm->set, NULL, &zero_ts);
                assert(res >= 0 || errno == EAGAIN);
            }
            else {
                break;
            }
        }
        else if(sm->bit_state == MBB_CLI_STATUS_DO_DELAY) {
            struct timer_notify_s notify;
            notify.pid      = getpid();
            notify.periodic = false;
            notify.event.sigev_notify = SIGEV_SIGNAL;
            notify.event.sigev_signo  = sm->signum;
            notify.event.sigev_value.sival_ptr = NULL;
            res = ioctl(sm->tim_fd, TCIOC_NOTIFICATION, (unsigned long)((uintptr_t)&notify));
            assert(res == 0);

            res = ioctl(sm->tim_fd, TCIOC_START, 0);
            assert(res == 0);
            break;
        }
        else assert(0);
    }

    return false;
}

static void master_sm_start_over(master_socket_sm_t * sm)
{
    mbb_cli_transfer_t transfer;
    if(sm->is_read) {
        transfer = MBB_CLI_BYTE_TRANSFER_READ;
    } else {
        uint8_t w_val = *sm->buf++;
        transfer = MBB_CLI_BYTE_TRANSFER_WRITE(w_val);
    }
    mbb_cli_start_byte_transfer(&sm->pin_soc.mbb, transfer);
    sm->len--;
}

static bool master_sm_next_byte_cb(pin_socket_ctx_t * pin_soc)
{
    master_socket_sm_t * sm = (master_socket_sm_t *) pin_soc;

    if(sm->is_read) {
        *sm->buf++ = mbb_cli_get_read_byte(&sm->pin_soc.mbb);
    }

    if(sm->len == 0) return true;

    master_sm_start_over(sm);

    return false;
}

static void multitasking_inner(socket_sms_t * s, uint8_t * buf, uint32_t len, bool is_read)
{
    int res;
    bool done;

    if(len == 0) return;

    s->s0.buf = buf;
    s->s0.len = len;
    s->s0.is_read = is_read;

    master_sm_start_over(&s->s0);

    done = sm_next(&s->s0.pin_soc);
    if(done) return;

    struct pollfd pfd;
    pfd.fd = s->sfd;
    pfd.events = POLLIN;
    while(1) {
        res = poll(&pfd, 1, -1);
        assert(res > 0);
        assert(pfd.revents == POLLIN);

        sigset_t set;
        res = sigpending(&set);
        assert(res == 0);

        res = sigismember(&set, s->s0.pin_soc.signum);
        assert(res >= 0);
        if(res) {
            done = sm_next(&s->s0.pin_soc);
            if(done) return;
        }

        res = sigismember(&set, s->s1.pin_soc.signum);
        assert(res >= 0);
        if(res) {
            sm_next(&s->s1.pin_soc);
        }
    }
}

static void multitasking_read(socket_sms_t * s, uint8_t * buf, uint32_t len)
{
    multitasking_inner(s, buf, len, true);
}

static void multitasking_write(socket_sms_t * s, uint8_t * buf, uint32_t len)
{
    multitasking_inner(s, buf, len, false);
}

static void continue_transfer(socket_sms_t * s, peer_data_t * pd, struct pollfd * pfd)
{
    ssize_t rwres;
    uint8_t buf[255];

    int fd = -pfd->fd;
    bool is_read = pd->is_doing == IS_READING;

    while (pd->transaction_remaining_len) {
        pd->was_unblocked = false;

        uint8_t try_to_move = MIN(pd->transaction_remaining_len, 255);

        buf[0] = is_read ? MMN_SRV_OPCODE_READ : MMN_SRV_OPCODE_WRITE;
        buf[1] = try_to_move;
        buf[2] = pd->token;

        multitasking_write(s, buf, 3);
        multitasking_read(s, buf, 1);

        uint8_t actually_move = MIN(try_to_move, buf[0]);

        if(!actually_move) {
            if(pd->was_unblocked) {
                continue;
            }
            break;
        }

        if(is_read) {
            multitasking_read(s, buf, actually_move);
            rwres = write(fd, buf, actually_move);
            assert(rwres == actually_move);
        } else {
            rwres = mcp_daemon_util_full_read(fd, buf, actually_move);
            assert(rwres == actually_move);
            multitasking_write(s, buf, actually_move);
        }
        pd->transaction_remaining_len -= actually_move;
    }
    if(!pd->transaction_remaining_len) {
        pd->is_doing = 0;
        pfd->fd = fd;
    }
}

int mcp_daemon_main(int argc, char *argv[])
{
    int res;
    ssize_t rwres;
    sigset_t set;

    usleep(100 * 1000); /* wait for backplane to start up */

    socket_sms_t s;

    pin_socket_ctx_init(&s.s0.pin_soc, "/dev/mcp0_clk", "/dev/mcp0_dat", "/dev/timer0", SIGUSR1, master_sm_next_byte_cb);
    pin_socket_ctx_init(&s.s1.pin_soc, "/dev/mcp1_clk", "/dev/mcp1_dat", "/dev/timer1", SIGUSR2, poller_sm_next_byte_cb);

    do_write(&s.s0.pin_soc, 255); /* assign me a token */
    uint8_t my_token = do_read(&s.s0.pin_soc);
    do_write(&s.s1.pin_soc, my_token); /* associate with other socket */

    run_socket(&s.s0.pin_soc, my_token);
    run_socket(&s.s1.pin_soc, my_token);

    s.s1.something_happened = false;
    s.s1.new_global_token_count = 0;
    s.s1.peer_datas = NULL;
    s.s1.peer_count = 0;
    poller_sm_start_over(&s.s1);
    sm_next(&s.s1.pin_soc);

    res = sigemptyset(&set);
    assert(res == 0);
    res = sigaddset(&set, s.s0.pin_soc.signum);
    assert(res == 0);
    res = sigaddset(&set, s.s1.pin_soc.signum);
    assert(res == 0);
    s.sfd = signalfd(-1, &set, SFD_CLOEXEC);
    assert(s.sfd >= 0);

    // nonblock is so `accept` doesn't block
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    assert(srv >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    assert(sizeof(SOC_PATH) <= sizeof(addr.sun_path));
    memcpy(addr.sun_path, SOC_PATH, sizeof(SOC_PATH));
    res = bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    assert(res == 0);
    res = listen(srv, 255);
    assert(res == 0);

    /* opening this fifo will unblock anyone blocked on opening it for writing */
    res = mkfifo(SOC_WAITER_FIFO, 0666);
    assert(res >= 0 || errno == EEXIST);
    int srv_fifo = open(SOC_WAITER_FIFO, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    assert(srv_fifo >= 0);

    nfds_t n_pollfds = POLLFDS_PEER_START;
    struct pollfd * pollfds = malloc(POLLFDS_PEER_START * sizeof(struct pollfd));
    assert(pollfds);

    pollfds[0].fd = s.sfd;
    pollfds[0].events = POLLIN;

    pollfds[1].fd = srv;
    pollfds[1].events = POLLIN;

    uint8_t global_token_count = 0;

    while(1) {
        while(s.s1.something_happened) {
            s.s1.something_happened = false;

            uint8_t tokens_to_add = s.s1.new_global_token_count - global_token_count;
            if (my_token >= global_token_count && my_token < s.s1.new_global_token_count) {
                tokens_to_add--;
            }
            uint8_t old_global_token_count = global_token_count;
            global_token_count = s.s1.new_global_token_count;
            if(tokens_to_add) {
                uint8_t old_peer_count = s.s1.peer_count;
                s.s1.peer_count += tokens_to_add;

                s.s1.peer_datas = realloc(s.s1.peer_datas, s.s1.peer_count * sizeof(peer_data_t));
                assert(s.s1.peer_datas);
                n_pollfds = POLLFDS_PEER_START + s.s1.peer_count;
                pollfds = realloc(pollfds, n_pollfds * sizeof(struct pollfd));
                assert(pollfds);

                uint8_t new_token = old_global_token_count;
                for(uint8_t i = old_peer_count; i < s.s1.peer_count; i++) {
                    if(new_token == my_token) new_token++;

                    peer_data_t * pd = &s.s1.peer_datas[i];
                    pd->token = new_token;
                    pd->is_doing = 0;

                    struct pollfd * pfd = &pollfds[POLLFDS_PEER_START + i];
                    pfd->fd = -1;
                    pfd->events = POLLIN;

                    new_token++;
                }

                for(uint8_t i = old_peer_count; i < s.s1.peer_count; i++) {
                    uint8_t set_interest_buf[] = {MMN_SRV_OPCODE_SET_INTEREST,
                                                  s.s1.peer_datas[i].token,
                                                  MMN_SRV_FLAG_READABLE | MMN_SRV_FLAG_WRITABLE};
                    multitasking_write(&s, set_interest_buf, sizeof(set_interest_buf));
                }
            }

            for(uint8_t i = 0; i < s.s1.peer_count; i++) {
                peer_data_t * pd = &s.s1.peer_datas[i];
                if(pd->is_doing && pd->was_unblocked) {
                    continue_transfer(&s, pd, &pollfds[POLLFDS_PEER_START + i]);
                }
            }
        }

        res = sigpending(&set);
        assert(res == 0);
        res = sigismember(&set, s.s1.pin_soc.signum);
        assert(res >= 0);
        if(res) {
            sm_next(&s.s1.pin_soc);
            continue;
        }

        res = poll(pollfds, n_pollfds, -1);
        assert(res > 0);
        int remaining_ready_fds = res;

        if(pollfds[0].revents) {
            assert(pollfds[0].revents == POLLIN);
            sm_next(&s.s1.pin_soc);
            if(--remaining_ready_fds == 0) continue;
        }

        if(pollfds[1].revents) {
            assert(pollfds[1].revents == POLLIN);

            int new_soc = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
            assert(new_soc >= 0);
            uint8_t for_token;
            rwres = read(new_soc, &for_token, 1);
            assert(rwres > 0);

            uint8_t response = RESULT_OK;

            uint8_t i;
            struct pollfd * pfd;
            for(i = 0; i < s.s1.peer_count; i++)
                if(s.s1.peer_datas[i].token == for_token) break;
            if(i == s.s1.peer_count) {
                response = RESULT_TOKEN_DOESNT_EXIST;
            } else {
                pfd = &pollfds[POLLFDS_PEER_START + i];
            }

            /* This pfd->fd != -1 is not the same as pfd->fd < 0
             * because a busy connection may set its fd
             * negative which means it's just disabled, not closed.
             * The only sentinel value meaning "vacant" here is -1.
             */
            if(response == RESULT_OK && pfd->fd != -1) {
                response = RESULT_MODULE_BUSY;
            }

            rwres = write(new_soc, &response, 1);
            assert(rwres > 0);

            if(response == RESULT_OK) {
                pfd->fd = new_soc;
            } else {
                res = close(new_soc);
                assert(res == 0);
            }

            if(--remaining_ready_fds == 0) continue;
        }

        for(uint8_t i = 0; i < s.s1.peer_count; i++) {
            struct pollfd * pfd = &pollfds[POLLFDS_PEER_START + i];
            if(!pfd->revents) continue;
            assert(!(pfd->revents & (POLLERR | POLLNVAL)));

            peer_data_t * pd = &s.s1.peer_datas[i];

            uint8_t operation;

            rwres = read(pfd->fd, &operation, 1);
            assert(rwres > 0);

            if(operation == OPERATION_QUIT) {
                uint8_t response = RESULT_OK;
                rwres = write(pfd->fd, &response, 1);
                assert(rwres > 0);

                res = close(pfd->fd);
                assert(res == 0);

                pfd->fd = -1;
            }
            else {

                pd->is_doing = operation;

                rwres = mcp_daemon_util_full_read(pfd->fd, &pd->transaction_remaining_len, 4);
                assert(rwres == 4);

                pfd->fd = -pfd->fd;

                continue_transfer(&s, pd, pfd);
            }

            if(--remaining_ready_fds == 0) break;
        }
    }

    // int peer_count_signed = s.s1.peer_count
    // for(int i = -1; i < peer_count_signed; i++) {
    //     int fd = pollfds[POLLFDS_PEER_START + i].fd;
    //     if(fd != -1) {
    //         res = close(ABS(fd));
    //         assert(res == 0);
    //     }
    // }

    free(pollfds);
    free(s.s1.peer_datas);

    // // close(srv_fifo);
    // close(srv);
    // close(s.sfd);
    // pin_socket_ctx_deinit(&s.s1.pin_soc);
    // pin_socket_ctx_deinit(&s.s0.pin_soc);

    return 0;
}

ssize_t mcp_daemon_util_full_read(int fd, void * buf, size_t count)
{
    size_t remain = count;
    uint8_t * buf_u8 = buf;
    while(remain) {
        ssize_t rwres = read(fd, buf_u8, remain);
        if (rwres < 0) return rwres;
        if (rwres == 0) break;
        buf_u8 += rwres;
        remain -= rwres;
    }
    return count - remain;
}
