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
#include <stdlib.h>
#include <stddef.h>

#include "mcpd_private.h"

struct resource_path_ent_s {
    struct resource_path_ent_s * next;
    uint8_t resource_id;
    char path[];
};

struct mcpd_con_s {
    int con;
    struct resource_path_ent_s * resource_path_head;
};

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

    mcpd_con_t conp = malloc(sizeof(*conp));
    assert(conp);
    conp->con = con;
    conp->resource_path_head = NULL;

    *con_dst = conp;
    return MCPD_OK;
}

void mcpd_disconnect(mcpd_con_t conp)
{
    int res;
    ssize_t rwres;

    int con = conp->con;

    uint8_t operation = OPERATION_QUIT;
    rwres = write(con, &operation, 1);
    assert(rwres > 0);

    uint8_t response;
    rwres = read(con, &response, 1);
    assert(rwres > 0);
    assert(response == RESULT_OK);

    res = close(con);
    assert(res == 0);

    struct resource_path_ent_s * path_ent = conp->resource_path_head;
    while(path_ent) {
        struct resource_path_ent_s * next = path_ent->next;
        free(path_ent);
        path_ent = next;
    }

    free(conp);
}

void mcpd_write(mcpd_con_t conp, const void * data, uint32_t len)
{
    ssize_t rwres;

    int con = conp->con;

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

void mcpd_read(mcpd_con_t conp, void * data, uint32_t len)
{
    ssize_t rwres;

    int con = conp->con;

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

int mcpd_gpio_acquire(mcpd_con_t conp, unsigned socketno, unsigned pinno)
{
    ssize_t rwres;

    int con = conp->con;

    uint8_t buf[] = {OPERATION_GPIO_ACQUIRE, socketno, pinno};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    rwres = read(con, buf, 1);
    assert(rwres > 0);

    if(buf[0] == 255) return MCPD_RESOURCE_UNAVAILABLE;
    return buf[0];
}

void mcpd_gpio_set(mcpd_con_t conp, unsigned gpio_id, bool en)
{
    ssize_t rwres;

    int con = conp->con;

    uint8_t buf[] = {OPERATION_GPIO_SET, gpio_id, en};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));
}

int mcpd_resource_acquire(mcpd_con_t conp, mcpd_pins_periph_type_t periph_type,
    mcpd_pins_driver_type_t driver_type)
{
    ssize_t rwres;

    int con = conp->con;

    uint8_t buf[] = {OPERATION_RESOURCE_ACQUIRE, periph_type, driver_type};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    rwres = read(con, buf, 1);
    assert(rwres > 0);

    if(buf[0] == 255) return MCPD_RESOURCE_UNAVAILABLE;
    return buf[0];
}

int mcpd_resource_route(mcpd_con_t conp, unsigned resource_id, unsigned io_type,
    unsigned socketno, unsigned pinno)
{
    ssize_t rwres;

    int con = conp->con;

    uint8_t buf[] = {OPERATION_RESOURCE_ROUTE, resource_id, io_type, socketno, pinno};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    rwres = read(con, buf, 1);
    assert(rwres > 0);

    return -buf[0];
}

const char * mcpd_resource_get_path(mcpd_con_t conp, unsigned resource_id)
{
    ssize_t rwres;

    int con = conp->con;

    struct resource_path_ent_s ** entp = &conp->resource_path_head;
    struct resource_path_ent_s * ent;
    while((ent = *entp)) {
        if(ent->resource_id == resource_id) {
            return ent->path;
        }
        entp = &ent->next;
    }

    uint8_t buf[] = {OPERATION_RESOURCE_GET_PATH, resource_id};

    rwres = write(con, buf, sizeof(buf));
    assert(rwres == sizeof(buf));

    uint8_t path_len;
    rwres = read(con, &path_len, 1);
    assert(rwres > 0);

    if(path_len == 0) return NULL;

    struct resource_path_ent_s * new_ent = malloc(offsetof(struct resource_path_ent_s, path) + path_len + 1);
    assert(new_ent);
    new_ent->next = NULL;
    new_ent->resource_id = resource_id;

    rwres = mcpd_util_full_read(con, new_ent->path, path_len);
    assert(rwres == path_len);
    new_ent->path[path_len] = '\0';

    *entp = new_ent;

    return new_ent->path;
}

int mcpd_file_hash(mcpd_con_t conp, const char * file_name, uint8_t * hash_32_byte_dst)
{
    uint8_t byte;

    size_t file_name_len = strlen(file_name);
    if(file_name_len > 255) {
        return MCPD_NAMETOOLONG;
    }

    byte = 2; /* hash protocol */
    mcpd_write(conp, &byte, 1);
    mcpd_read(conp, &byte, 1);
    if(byte) {
        return MCPD_PROTOCOL_NOT_SUP;
    }

    byte = file_name_len;
    mcpd_write(conp, &byte, 1);
    mcpd_write(conp, file_name, file_name_len);

    mcpd_read(conp, &byte, 1);

    switch(byte) {
        case 0: break;
        case 1: return MCPD_IOERROR;
        case 3: return MCPD_NOENT;
        case 4: return MCPD_NAMETOOLONG;
        default: assert(0);
    }

    mcpd_read(conp, hash_32_byte_dst, 32);

    return 0;
}
