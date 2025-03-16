#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/uio.h>
#include <assert.h>

#include "mcp_daemon_private.h"

int mcp_daemon_connect(mcp_daemon_con_t * con_dst, int peer_id)
{
    int res;
    ssize_t rwres;

    int con = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(con >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    assert(sizeof(SOC_PATH) <= sizeof(addr.sun_path));
    memcpy(addr.sun_path, SOC_PATH, sizeof(SOC_PATH));

    res = connect(con, (struct sockaddr *) &addr, sizeof(addr));
    assert(res == 0);

    uint8_t token = peer_id;
    rwres = write(con, &token, 1);
    assert(rwres > 0);

    uint8_t response;
    rwres = read(con, &response, 1);
    assert(rwres > 0);

    if(response != RESULT_OK) {
        res = close(con);
        assert(res == 0);

        int ret = response;
        return -ret;
    }

    *con_dst = con;
    return MCP_DAEMON_OK;
}

void mcp_daemon_disconnect(mcp_daemon_con_t con)
{
    int res;
    ssize_t rwres;

    uint8_t operation = OPERATION_QUIT;
    rwres = write(con, &operation, 1);
    assert(rwres > 0);

    uint8_t response;
    rwres = read(con, &response, 1);
    assert(rwres > 0);
    assert(response == RESULT_OK);

    res = close(con);
    assert(res == 0);
}

void mcp_daemon_write(mcp_daemon_con_t con, const void * data, uint32_t len)
{
    ssize_t rwres;

    if(len == 0) return;

    uint8_t operation = OPERATION_WRITE;

    struct iovec v[3] = {
        {.iov_base = &operation, .iov_len = 1},
        {.iov_base = &len, .iov_len = 4},
        {.iov_base = (void *) data, .iov_len = len}
    };

    rwres = writev(con, v, 3);
    assert(rwres == 5 + len);
}

void mcp_daemon_read(mcp_daemon_con_t con, void * data, uint32_t len)
{
    ssize_t rwres;

    if(len == 0) return;

    uint8_t operation = OPERATION_READ;

    struct iovec v[2] = {
        {.iov_base = &operation, .iov_len = 1},
        {.iov_base = &len, .iov_len = 4}
    };

    rwres = writev(con, v, 2);
    assert(rwres == 5);

    rwres = mcp_daemon_util_full_read(con, data, len);
    assert(rwres == len);
}
