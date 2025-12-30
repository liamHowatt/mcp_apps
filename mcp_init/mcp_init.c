#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <assert.h>
#include <spawn.h>
#include <stdlib.h>

#include <mcp/mcpd.h>

static void run_forth(int peer_id)
{
    int res;
    char path[32];
    char peer[16];
    pid_t pid;

    char prog_name[] = "mcp_forth";
    char opt_fl[] = "-O";
    snprintf(path, sizeof(path), "/mnt/mcp/%d/main.4th", peer_id);
    char * task_argv[] = {prog_name, opt_fl, path, NULL};

    snprintf(peer, sizeof(peer), "%d", peer_id);
    res = setenv("MCP_PEER", peer, 1);
    assert(res == 0);

    res = posix_spawn(&pid, "mcp_forth", NULL, NULL, task_argv, NULL);
    assert(res >= 0);
}

int mcp_init_main(int argc, char *argv[])
{
    int res;
    pid_t pid;

    res = posix_spawn(&pid, "mcpd", NULL, NULL, NULL, NULL);
    assert(res >= 0);

    res = posix_spawn(&pid, "mcp_fs", NULL, NULL, NULL, NULL);
    assert(res >= 0);

    char prog_name[] = "mcp_lvgl";
    char opt_fl[] = "-O";
    char * task_argv[] = {prog_name, opt_fl, NULL};
    res = posix_spawn(&pid, "mcp_lvgl", NULL, NULL, task_argv, NULL);
    assert(res >= 0);

    mcpd_watch_t watch = mcpd_watch_create();

    while(1) {
        int peer = mcpd_watch_wait(watch);
        assert(peer >= 0);

        run_forth(peer);
    }

    /* mcpd_watch_destroy(watch); */

    return 0;
}
