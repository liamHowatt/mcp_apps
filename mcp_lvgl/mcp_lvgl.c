#include <stdio.h>
#include <fcntl.h>
#include <mqueue.h>
#include <unistd.h>

#include "mcp/mcp_lvgl.h"

static mqd_t inner_open(int oflag)
{
    struct mq_attr attr = {.mq_maxmsg = 16, .mq_msgsize = 32};
    return mq_open("mcp_lvgl", oflag, 0666, &attr);
}

mcp_lvgl_queue_t mcp_lvgl_open_queue(void)
{
    return inner_open(O_WRONLY | O_CREAT);
}

bool mcp_lvgl_queue_open_was_ok(mcp_lvgl_queue_t mqd)
{
    return mqd != (mqd_t)-1;
}

void mcp_lvgl_queue_send(mcp_lvgl_queue_t, const char *);

void mcp_lvgl_queue_close(mcp_lvgl_queue_t mqd)
{
    mq_close(mqd);
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
