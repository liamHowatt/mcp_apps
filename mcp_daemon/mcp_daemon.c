#include "mcp_board/mcp_bitbang/mcp_bitbang_client.h"

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

typedef struct {
    mbb_cli_t mbb;
    int clk_fd;
    int dat_fd;
    int tim_fd;
    int signum;
    bool clk_val;
    bool dat_val;
} pin_socket_ctx_t;

static void pin_set(void * vctx, mbb_cli_pin_t pinno, bool val) {
    int res;

    pin_socket_ctx_t * ctx = vctx;
    bool * cur_val = pinno == MBB_CLI_PIN_CLK ? &ctx->clk_val : &ctx->dat_val;
    if(val == *cur_val) return;
    int fd = pinno == MBB_CLI_PIN_CLK ? ctx->clk_fd : ctx->dat_fd;
    res = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long) (val ? GPIO_INTERRUPT_RISING_PIN : GPIO_OUTPUT_PIN));
    assert(res >= 0);
    *cur_val = val;

}

static bool pin_get(void * vctx, mbb_cli_pin_t pinno)
{
    int res;

    pin_socket_ctx_t * ctx = vctx;
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

static void pin_wait(void * vctx, mbb_cli_pin_t pinno)
{
    int res;

    pin_socket_ctx_t * ctx = vctx;
    bool output_val = pinno == MBB_CLI_PIN_CLK ? ctx->clk_val : ctx->dat_val;
    assert(output_val);
    int fd = pinno == MBB_CLI_PIN_CLK ? ctx->clk_fd : ctx->dat_fd;

    sigset_t set;
    sigset_t oldset;
    sigemptyset(&set);
    sigaddset(&set, ctx->signum);
    res = sigprocmask(SIG_BLOCK, &set, &oldset);
    assert(res == 0);

    struct sigevent notify;
    notify.sigev_notify = SIGEV_SIGNAL;
    notify.sigev_signo  = ctx->signum;

    res = ioctl(fd, GPIOC_REGISTER, (unsigned long)&notify);
    assert(res >= 0);

    bool already_high = pin_get(ctx, pinno);

    if(!already_high) {
        res = sigwaitinfo(&set, NULL);
        assert(res != -1);
    }

    ioctl(fd, GPIOC_UNREGISTER, 0);
    assert(res >= 0);

    if(already_high) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 0};
        res = sigtimedwait(&set, NULL, &ts);
        assert(res != -1 || errno == EAGAIN);
    }

    res = sigprocmask(SIG_SETMASK, &oldset, NULL);
    assert(res == 0);
}

static void timer_sleep(pin_socket_ctx_t * ctx)
{
    int res;

    sigset_t set;
    sigset_t oldset;
    sigemptyset(&set);
    sigaddset(&set, ctx->signum);
    res = sigprocmask(SIG_BLOCK, &set, &oldset);
    assert(res == 0);

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
    res = sigwaitinfo(&set, NULL);
    assert(res != -1);

    res = sigprocmask(SIG_SETMASK, &oldset, NULL);
    assert(res == 0);
}

static void run_transfer(pin_socket_ctx_t * ctx)
{
    mbb_cli_status_t status;
    while (MBB_CLI_STATUS_DONE != (status = mbb_cli_continue_byte_transfer(&ctx->mbb))) {
        switch (status) {
            case MBB_CLI_STATUS_DO_DELAY:
                timer_sleep(ctx);
                break;
            case MBB_CLI_STATUS_DO_DELAY_AND_WAIT_CLK_PIN_HIGH:
                timer_sleep(ctx);
                /* fallthrough */
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
                                const char * tim_path, int signum)
{
    int res;

    ctx->clk_fd = open(clk_path, O_RDWR);
    assert(ctx->clk_fd != -1);
    ctx->dat_fd = open(dat_path, O_RDWR);
    assert(ctx->dat_fd != -1);

    ctx->tim_fd = open(tim_path, O_RDONLY);
    assert(ctx->tim_fd != -1);
    res = ioctl(ctx->tim_fd, TCIOC_SETTIMEOUT, 500); /* 500 us */
    assert(res == 0);

    ctx->signum = signum;
    
    ctx->clk_val = false;
    ctx->dat_val = false;

    mbb_cli_init(&ctx->mbb, pin_get, pin_set, ctx);

    pin_set(ctx, MBB_CLI_PIN_CLK, 1);
    pin_set(ctx, MBB_CLI_PIN_DAT, 1);

    pin_wait(ctx, MBB_CLI_PIN_CLK);
    pin_wait(ctx, MBB_CLI_PIN_DAT);

    timer_sleep(ctx);
    pin_set(ctx, MBB_CLI_PIN_CLK, 0);
    timer_sleep(ctx);
}

static void pin_socket_ctx_deinit(pin_socket_ctx_t * ctx)
{
    close(ctx->clk_fd);
    close(ctx->dat_fd);
    close(ctx->tim_fd);
}

static void run_socket(pin_socket_ctx_t * ctx)
{
    do_write(ctx, 255);
    uint8_t token = do_read(ctx);
    printf("token: %d\n", (int) token);

    do_write(ctx, 5); // whereami
    uint8_t where = do_read(ctx);
    printf("where: %d\n", (int) where);

    do_write(ctx, 0); // write
    do_write(ctx, 4); // "cpu4" is 4 bytes
    do_write(ctx, token); // send to self
    uint8_t free_space = do_read(ctx);
    printf("free space: %d\n", (int) free_space);
    assert(free_space >= 4);
    do_write(ctx, 'c');
    do_write(ctx, 'p');
    do_write(ctx, 'u');
    do_write(ctx, '4');

    do_write(ctx, 1); // read
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

int mcp_daemon_main(int argc, char *argv[])
{
    pin_socket_ctx_t ctx[2];

    pin_socket_ctx_init(&ctx[0], "/dev/mcp0_clk", "/dev/mcp0_dat", "/dev/timer0", SIGUSR1);
    pin_socket_ctx_init(&ctx[1], "/dev/mcp1_clk", "/dev/mcp1_dat", "/dev/timer1", SIGUSR2);

    run_socket(&ctx[0]);
    run_socket(&ctx[1]);

    pin_socket_ctx_deinit(&ctx[0]);
    pin_socket_ctx_deinit(&ctx[1]);

    return 0;
}
