#include "mcp_board/mcp_bitbang/mcp_bitbang_client.h"

#include <nuttx/ioexpander/gpio.h>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>

static void pin_set(void * vctx, mbb_cli_pin_t pinno, bool val) {
    int * ctx = vctx;
    int fd = ctx[pinno];
    int res = ioctl(fd, GPIOC_SETPINTYPE, (unsigned long) (val ? GPIO_INTERRUPT_RISING_PIN : GPIO_OUTPUT_PIN));
    assert(res >= 0);
}

static bool pin_get(void * vctx, mbb_cli_pin_t pinno) {
    int * ctx = vctx;
    int fd = ctx[pinno];
    bool val;
    int res = ioctl(fd, GPIOC_READ, (unsigned long)((uintptr_t)&val));
    assert(res >= 0);
    return val;
}

static void pin_wait(void * vctx, mbb_cli_pin_t pinno, int signum)
{
    int * ctx = vctx;
    int fd = ctx[pinno];

    sigset_t set;
    sigset_t oldset;
    sigemptyset(&set);
    sigaddset(&set, signum);
    int res = sigprocmask(SIG_BLOCK, &set, &oldset);
    assert(res == 0);

    struct sigevent notify;
    notify.sigev_notify = SIGEV_SIGNAL;
    notify.sigev_signo  = signum;

    res = ioctl(fd, GPIOC_REGISTER, (unsigned long)&notify);
    assert(res >= 0);

    if(!pin_get(ctx, pinno)) {
        res = sigwaitinfo(&set, NULL);
        assert(res != -1);
    }

    ioctl(fd, GPIOC_UNREGISTER, 0);
    assert(res >= 0);

    res = sigprocmask(SIG_SETMASK, &oldset, NULL);
    assert(res == 0);
}

static void run_transfer(mbb_cli_t * cli)
{
    mbb_cli_status_t status;
    while (MBB_CLI_STATUS_DONE != (status = mbb_cli_continue_byte_transfer(cli))) {
        switch (status) {
            case MBB_CLI_STATUS_DO_DELAY:
                usleep(2 * 1000);
                break;
            case MBB_CLI_STATUS_DO_DELAY_AND_WAIT_CLK_PIN_HIGH:
                usleep(2 * 1000);
                /* fallthrough */
            case MBB_CLI_STATUS_WAIT_CLK_PIN_HIGH:
                pin_wait(cli->caller_ctx, MBB_CLI_PIN_CLK, SIGUSR1);
                break;
            default:
                assert(0);
        }
    }
}

static uint8_t do_read(mbb_cli_t * cli)
{
    mbb_cli_start_byte_transfer(cli, MBB_CLI_BYTE_TRANSFER_READ);
    run_transfer(cli);
    return mbb_cli_get_read_byte(cli);
}

static void do_write(mbb_cli_t * cli, uint8_t data)
{
    mbb_cli_start_byte_transfer(cli, MBB_CLI_BYTE_TRANSFER_WRITE(data));
    run_transfer(cli);
}

static void run_socket(const char * clk_path, const char * dat_path)
{
    int clk_fd = open(clk_path, O_RDWR);
    assert(clk_fd != -1);
    int dat_fd = open(dat_path, O_RDWR);
    assert(dat_fd != -1);

    int ctx[2] = {clk_fd, dat_fd};

    pin_set(ctx, MBB_CLI_PIN_CLK, 1);
    pin_set(ctx, MBB_CLI_PIN_DAT, 1);

    pin_wait(ctx, MBB_CLI_PIN_CLK, SIGUSR1);
    pin_wait(ctx, MBB_CLI_PIN_DAT, SIGUSR1);

    usleep(2 * 1000);
    pin_set(ctx, MBB_CLI_PIN_CLK, 1);
    usleep(2 * 1000);
    pin_set(ctx, MBB_CLI_PIN_CLK, 0);
    usleep(2 * 1000);

    static mbb_cli_t cli;
    mbb_cli_init(&cli, pin_get, pin_set, ctx);

    do_write(&cli, 255);
    uint8_t token = do_read(&cli);
    printf("token: %d\n", (int) token);

    do_write(&cli, 5); // whereami
    uint8_t where = do_read(&cli);
    printf("where: %d\n", (int) where);

    do_write(&cli, 0); // write
    do_write(&cli, 4); // "cpu4" is 4 bytes
    do_write(&cli, token); // send to self
    uint8_t free_space = do_read(&cli);
    printf("free space: %d\n", (int) free_space);
    assert(free_space >= 4);
    do_write(&cli, 'c');
    do_write(&cli, 'p');
    do_write(&cli, 'u');
    do_write(&cli, '4');

    do_write(&cli, 1); // read
    do_write(&cli, 4); // "cpu4" is 4 bytes
    do_write(&cli, token); // recv from self
    uint8_t readable = do_read(&cli);
    printf("amount readable: %d\n", (int) readable);
    assert(readable == 4);
    assert(do_read(&cli) == 'c');
    assert(do_read(&cli) == 'p');
    assert(do_read(&cli) == 'u');
    assert(do_read(&cli) == '4');
    printf("'cpu4' received\n");

    close(clk_fd);
    close(dat_fd);
}

int mcp_daemon_main(int argc, char *argv[])
{
    run_socket("/dev/mcp0_clk", "/dev/mcp0_dat");
    run_socket("/dev/mcp1_clk", "/dev/mcp1_dat");
    return 0;
}
