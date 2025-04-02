#include <stdio.h>
#include <stdlib.h>

#include <mcp/mcp_lvgl.h>

int custom_hello_main(int argc, char *argv[])
{
    if(argc < 2) return 1;

    mcp_lvgl_queue_t q = mcp_lvgl_queue_open();
    mcp_lvgl_queue_send(q, argv[1]);
    mcp_lvgl_queue_close(q);

    return 0;
}
