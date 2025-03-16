#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

#include <mcp/mcp_lvgl.h>

static mqd_t inner_open(int oflag)
{
    struct mq_attr attr = {.mq_maxmsg = 16, .mq_msgsize = 32};
    mqd_t q = mq_open("mcp_lvgl", oflag, 0666, &attr);
    assert(q != -1);
    return q;
}

mcp_lvgl_queue_t mcp_lvgl_open_queue(void)
{
    return inner_open(O_WRONLY | O_CREAT);
}

void mcp_lvgl_queue_send(mcp_lvgl_queue_t, const char *);

void mcp_lvgl_queue_close(mcp_lvgl_queue_t mqd)
{
    int res = mq_close(mqd);
    assert(res == 0);
}

int mcp_lvgl_main(int argc, char *argv[])
{
    puts("mcp lvgl");

    inner_open(O_RDONLY | O_CREAT | O_NONBLOCK);

    while(1) {
        sleep(10);
    }

    return 0;
}
