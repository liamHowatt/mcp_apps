#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/uio.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include "mcpd_private.h"
#include <arch/board/mcp/mcp_pins_array.h>

int mcpd_connect(mcpd_con_t * con_dst, int peer_id)
{
    int res;
    ssize_t rwres;

    /* opening this fifo will block until the daemon has opened it for reading */
    res = mkfifo(SOC_WAITER_FIFO, 0666);
    assert(res >= 0 || errno == EEXIST);
    int srv_fifo = open(SOC_WAITER_FIFO, O_WRONLY | O_CLOEXEC);
    assert(srv_fifo >= 0);
    res = close(srv_fifo);
    assert(res == 0);

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
    return MCPD_OK;
}

void mcpd_disconnect(mcpd_con_t con)
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

void mcpd_write(mcpd_con_t con, const void * data, uint32_t len)
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

void mcpd_read(mcpd_con_t con, void * data, uint32_t len)
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

    rwres = mcpd_util_full_read(con, data, len);
    assert(rwres == len);
}

int mcpd_gpio_acquire(mcpd_con_t con, unsigned socketno, unsigned pinno)
{
    ssize_t rwres;

    uint8_t buf[] = {OPERATION_GPIO_ACQUIRE, socketno, pinno};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    rwres = read(con, buf, 1);
    assert(rwres > 0);

    if(buf[0] == 255) return MCPD_RESOURCE_UNAVAILABLE;
    return buf[0];
}

void mcpd_gpio_set(mcpd_con_t con, unsigned gpio_id, bool en)
{
    ssize_t rwres;

    uint8_t buf[] = {OPERATION_GPIO_SET, gpio_id, en};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));
}

int mcpd_resource_acquire(mcpd_con_t con, mcpd_pins_type_t type)
{
    ssize_t rwres;

    uint8_t buf[] = {OPERATION_RESOURCE_ACQUIRE, type};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    rwres = read(con, buf, 1);
    assert(rwres > 0);

    if(buf[0] == 255) return MCPD_RESOURCE_UNAVAILABLE;
    return buf[0];
}

int mcpd_resource_route(mcpd_con_t con, unsigned resource_id, unsigned io_type,
    unsigned socketno, unsigned pinno)
{
    ssize_t rwres;

    uint8_t buf[] = {OPERATION_RESOURCE_ROUTE, resource_id, io_type, socketno, pinno};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    rwres = read(con, buf, 1);
    assert(rwres > 0);

    return -buf[0];
}

const char * mcpd_resource_get_path(mcpd_con_t con, unsigned resource_id)
{
    if(resource_id >= MCP_PINS_COUNT) return NULL;
    return mcp_pins[resource_id].path;
}
