#include <mcp/mcp_daemon.h>
#include <nuttx/fs/userfs.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <fcntl.h>

#define FS_BASE_ACTION_WRITE      0
#define FS_BASE_ACTION_READ       1
#define FS_BASE_ACTION_LS         2
#define FS_BASE_ACTION_DELETE     3

#define FS_OPEN_FILE_ACTION_CONTINUE   0
#define FS_OPEN_FILE_ACTION_CLOSE      1
#define FS_OPEN_FILE_ACTION_STAT       2

typedef struct {
    mcp_daemon_con_t con;
    int refcount;
    bool has_been_connected_to;
    bool is_reading;
} peer_t;

typedef struct {
    int peer_count;
    int self_index;
    peer_t * peers;
    int open_dir_count; /*for asserting*/
    bool was_destroyed; /*for asserting*/
} volinfo_t;

typedef struct {
    bool is_root;
    int count;
    int cursor;
    char fnames[];
} dir_t;

static peer_t * volinfo_ensure_peer(volinfo_t * vinfo, int peer_id)
{
    if(peer_id < vinfo->peer_count) return &vinfo->peers[peer_id];
    int new_peer_count = peer_id + 1;
    vinfo->peers = realloc(vinfo->peers, new_peer_count * sizeof(peer_t));
    assert(vinfo->peers);
    for(int i = vinfo->peer_count; i < new_peer_count; i++) {
        vinfo->peers[i].con = MCP_DAEMON_CON_NULL;
        vinfo->peers[i].has_been_connected_to = false;
    }
    vinfo->peer_count = new_peer_count;
    return &vinfo->peers[peer_id];
}

static int decode_path(const char ** srcdst)
{
    int id = 0;
    const char * p = *srcdst;
    bool decoded_at_least_one_digit = false;
    char c;
    while(1) {
        c = *p;
        if(c < '0' || c > '9') break;
        p++;
        id *= 10;
        id += c - '0';
        if(id >= 255) return -1;

        decoded_at_least_one_digit = true;
    }
    if(!decoded_at_least_one_digit) return -1;
    if(c == '/') p++;
    else if(c != '\0') return -1;
    if(NULL != strchr(p, '/')) return -1;

    *srcdst = p;
    return id;
}

static bool check_peer_is_present(int peer_id)
{
    int res;

    mcp_daemon_con_t con;
    res = mcp_daemon_connect(&con, peer_id);

    if(res == MCP_DAEMON_DOESNT_EXIST) {
        return false;
    }

    if(res == MCP_DAEMON_OK) {
        mcp_daemon_disconnect(con);
    } else {
        assert(res == MCP_DAEMON_BUSY);
    }

    return true;
}

static int op_open(FAR void *volinfo, FAR const char *relpath,
    int oflags, mode_t mode, FAR void **openinfo)
{
    // mode is ignored

    int res;

    volinfo_t * vinfo = volinfo;

    int accmode = oflags & O_ACCMODE;
    if(!accmode) return -EINVAL;

    if(accmode == O_RDWR
       || (accmode == O_WRONLY
           && (oflags & (O_APPEND | O_CREAT | O_EXCL | O_TRUNC))
               != (O_CREAT | O_TRUNC))) return -ENOTSUP;

    if(*relpath == '\0') return -EISDIR;

    const char * p = relpath;
    int peer_id = decode_path(&p);

    if(peer_id < 0) return -ENOENT;

    if(peer_id == vinfo->self_index) return -ENOENT;

    if(*p == '\0') return -EISDIR;

    peer_t * peer = peer_id < vinfo->peer_count ? &vinfo->peers[peer_id] : NULL;

    if(peer && peer->con != MCP_DAEMON_CON_NULL) return -EBUSY;

    size_t filename_len = strlen(p);
    if(filename_len > 255) return -ENAMETOOLONG;

    mcp_daemon_con_t con;
    res = mcp_daemon_connect(&con, peer_id);
    if(res == MCP_DAEMON_DOESNT_EXIST) return -ENOENT;

    peer = volinfo_ensure_peer(vinfo, peer_id);
    peer->has_been_connected_to = true;

    if(res == MCP_DAEMON_BUSY) return -EBUSY;
    assert(res == MCP_DAEMON_OK);

    uint8_t buf[2];

    buf[0] = 0; // protocol
    mcp_daemon_write(con, buf, 1);
    mcp_daemon_read(con, buf, 1);
    if(buf[0] != 0) { // protocol not supported
        mcp_daemon_disconnect(con);
        return accmode == O_RDONLY ? -ENOENT : -EROFS;
    }

    buf[0] = accmode == O_RDONLY;
    buf[1] = filename_len;
    mcp_daemon_write(con, buf, 2);
    mcp_daemon_write(con, p, filename_len);
    mcp_daemon_read(con, buf, 1);
    if(buf[0]) {
        mcp_daemon_disconnect(con);
        switch(buf[0]) {
            case 1: return -EIO;
            case 2: return -EACCES;
            case 3: return -ENOENT;
            case 4: return -ENAMETOOLONG;
            case 5: return -ENOSPC;
            case 6: return -EROFS;
        }
        assert(0);
    }

    peer->con = con;
    peer->refcount = 1;
    peer->is_reading = accmode == O_RDONLY;
    *openinfo = peer;
    return 0;
}

static int op_close(FAR void *volinfo, FAR void *openinfo)
{
    peer_t * peer = openinfo;

    if(--peer->refcount) return 0;

    uint8_t buf[1];

    buf[0] = FS_OPEN_FILE_ACTION_CLOSE;
    mcp_daemon_write(peer->con, buf, 1);
    mcp_daemon_read(peer->con, buf, 1);
    mcp_daemon_disconnect(peer->con);
    peer->con = MCP_DAEMON_CON_NULL;

    switch(buf[0]) {
        case 0: break;
        case 1: return -EIO;
        default: assert(0);
    }

    return 0;
}

static ssize_t op_read(FAR void *volinfo, FAR void *openinfo,
    FAR char *buffer, size_t buflen)
{
    peer_t * peer = openinfo;

    if(!peer->is_reading) return -EBADF;

    uint8_t buf[5];
    buf[0] = FS_OPEN_FILE_ACTION_CONTINUE;
    uint32_t buflen_u32 = buflen;
    memcpy(buf + 1, &buflen_u32, 4);
    mcp_daemon_write(peer->con, buf, 5);

    uint32_t read_amount = 0;
    uint32_t chunk;

    while(buflen_u32) {
        mcp_daemon_read(peer->con, &chunk, 4);
        if(!chunk) break;
        assert(chunk <= buflen_u32);
        mcp_daemon_read(peer->con, buffer, chunk);
        buffer += chunk;
        buflen_u32 -= chunk;
        read_amount += chunk;
    }

    uint8_t result;
    mcp_daemon_read(peer->con, &result, 1);
    switch(result) {
        case 0: break;
        case 1: return -EIO;
        default: assert(0);
    }

    return read_amount;
}

static ssize_t op_write(FAR void *volinfo, FAR void *openinfo,
    FAR const char *buffer, size_t buflen)
{
    peer_t * peer = openinfo;

    if(peer->is_reading) return -EBADF;

    uint8_t buf[5];

    buf[0] = FS_OPEN_FILE_ACTION_CONTINUE;
    uint32_t lenu32 = buflen;
    memcpy(buf + 1, &lenu32, 4);
    mcp_daemon_write(peer->con, buf, 5);
    mcp_daemon_write(peer->con, buffer, lenu32);
    mcp_daemon_read(peer->con, buf, 1);

    switch(buf[0]) {
        case 0: break;
        case 1: return -EIO;
        case 5: return -ENOSPC;
        default: assert(0);
    }

    return buflen;
}

static off_t op_seek(FAR void *volinfo, FAR void *openinfo,
    off_t offset, int whence)
{
    return -ENOTSUP;
}

static int op_ioctl(FAR void *volinfo, FAR void *openinfo, int cmd,
    unsigned long arg)
{
    return -ENOTTY;
}

static int op_sync(FAR void *volinfo, FAR void *openinfo)
{
    return 0;
}

static int op_dup(FAR void *volinfo, FAR void *oldinfo,
    FAR void **newinfo)
{
    peer_t * peer = oldinfo;

    peer->refcount++;

    *newinfo = oldinfo;
    return 0;
}

static int op_fstat(FAR void *volinfo, FAR void *openinfo,
    FAR struct stat *statbuf)
{
    peer_t * peer = openinfo;

    memset(statbuf, 0, sizeof(*statbuf));

    uint8_t buf[1 + 2 + 4 + 2];
    buf[0] = FS_OPEN_FILE_ACTION_STAT;
    mcp_daemon_write(peer->con, buf, 1);
    mcp_daemon_read(peer->con, buf, sizeof(buf));

    if(buf[0] == 1) return -EIO;
    assert(buf[0] == 0);

    uint16_t mode;
    memcpy(&mode, buf + 1, 2);
    statbuf->st_mode = mode | S_IFREG;

    uint32_t size;
    memcpy(&size, buf + 3, 4);
    statbuf->st_size = size;

    uint16_t blksize;
    memcpy(&blksize, buf + 7, 2);
    statbuf->st_blksize = blksize;

    return 0;
}

static int op_truncate(FAR void *volinfo, FAR void *openinfo,
    off_t length)
{
    return -ENOTSUP;
}

static int op_opendir(FAR void *volinfo, FAR const char *relpath,
    FAR void **dir_dst)
{
    int res;

    volinfo_t * vinfo = volinfo;

    if(*relpath == '\0') {
        for(int i = 0; i < vinfo->peer_count; i++) {
            peer_t * peer = &vinfo->peers[i];
            if(peer->con == MCP_DAEMON_CON_NULL
               && !peer->has_been_connected_to
               && i != vinfo->self_index) {
                if(check_peer_is_present(i)) {
                    peer->has_been_connected_to = true;
                } else {
                    assert(vinfo->self_index < 0);
                    vinfo->self_index = i;
                }
            }
        }
        do {
            if(check_peer_is_present(vinfo->peer_count)) {
                peer_t * peer = volinfo_ensure_peer(vinfo, vinfo->peer_count);
                peer->has_been_connected_to = true;
            }
            else if(vinfo->self_index < 0) {
                vinfo->self_index = vinfo->peer_count;
                volinfo_ensure_peer(vinfo, vinfo->peer_count);
            }
        } while(vinfo->self_index < 0);

        dir_t * dir = malloc(sizeof(dir_t));
        assert(dir);
        dir->is_root = true;
        dir->count = vinfo->peer_count;
        dir->cursor = 0;

        vinfo->open_dir_count++;
        *dir_dst = dir;
        return 0;
    }

    const char * p = relpath;
    int peer_id = decode_path(&p);

    if(peer_id < 0 || peer_id == vinfo->self_index) return -ENOENT;
    if(*p != '\0') return -ENOTDIR;

    peer_t * peer = peer_id < vinfo->peer_count ? &vinfo->peers[peer_id] : NULL;
    if(peer && peer->con != MCP_DAEMON_CON_NULL) return -EBUSY;

    mcp_daemon_con_t con;
    res = mcp_daemon_connect(&con, peer_id);
    if(res == MCP_DAEMON_DOESNT_EXIST) return -ENOENT;

    peer = volinfo_ensure_peer(vinfo, peer_id);
    peer->has_been_connected_to = true;

    if(res == MCP_DAEMON_BUSY) return -EBUSY;
    assert(res == MCP_DAEMON_OK);

    uint8_t buf[1];

    buf[0] = 0; // protocol
    mcp_daemon_write(con, buf, 1);
    mcp_daemon_read(con, buf, 1);

    uint32_t byte_count;
    if(buf[0] != 0) { // protocol not supported
        byte_count = 0;
    } else {
        buf[0] = FS_BASE_ACTION_LS;
        mcp_daemon_write(con, buf, 1);
        mcp_daemon_read(con, &byte_count, sizeof(byte_count));
    }

    dir_t * dir = malloc(sizeof(dir_t) + byte_count);
    assert(dir);
    dir->is_root = false;
    dir->count = byte_count;
    dir->cursor = 0;

    mcp_daemon_read(con, dir->fnames, byte_count);

    mcp_daemon_disconnect(con);

    vinfo->open_dir_count++;
    *dir_dst = dir;
    return 0;
}

static int op_closedir(FAR void *volinfo, FAR void *dir)
{
    volinfo_t * vinfo = volinfo;
    vinfo->open_dir_count--;
    free(dir);
    return 0;
}

static int op_readdir(FAR void *volinfo, FAR void *dir_void,
    FAR struct dirent *entry)
{
    int res;

    volinfo_t * vinfo = volinfo;
    dir_t * dir = dir_void;

    if(dir->is_root && dir->cursor == vinfo->self_index) dir->cursor++;

    if(dir->cursor >= dir->count) return -ENOENT;

    memset(entry, 0, sizeof(*entry));

    if(dir->is_root) {
        entry->d_type = DT_DIR;
        res = snprintf(entry->d_name, sizeof(entry->d_name), "%d", dir->cursor);
        assert(res < sizeof(entry->d_name));
        dir->cursor++;
    }
    else {
        entry->d_type = DT_REG;
        size_t strlcpy_res = strlcpy(entry->d_name, dir->fnames + dir->cursor, sizeof(entry->d_name));
        assert(strlcpy_res < sizeof(entry->d_name));
        dir->cursor += strlcpy_res + 1;
    }

    return 0;
}

static int op_rewinddir(FAR void *volinfo, FAR void *dir_void)
{
    dir_t * dir = dir_void;

    dir->cursor = 0;
    return 0;
}

static int op_statfs(FAR void *volinfo, FAR struct statfs *buf)
{
    memset(buf, 0, sizeof(*buf));

    buf->f_type = USERFS_MAGIC;
    buf->f_namelen = 255;

    return 0;
}

static int op_unlink(FAR void *volinfo, FAR const char *relpath)
{
    int res;

    volinfo_t * vinfo = volinfo;

    if(*relpath == '\0') return -EPERM;

    const char * p = relpath;
    int peer_id = decode_path(&p);

    if(peer_id < 0 || peer_id == vinfo->self_index) return -ENOENT;
    if(*p == '\0') return -EPERM;

    peer_t * peer = peer_id < vinfo->peer_count ? &vinfo->peers[peer_id] : NULL;
    if(peer && peer->con != MCP_DAEMON_CON_NULL) return -EBUSY;

    size_t filename_len = strlen(p);
    if(filename_len > 255) return -ENAMETOOLONG;

    mcp_daemon_con_t con;
    res = mcp_daemon_connect(&con, peer_id);
    if(res == MCP_DAEMON_DOESNT_EXIST) return -ENOENT;

    peer = volinfo_ensure_peer(vinfo, peer_id);
    peer->has_been_connected_to = true;

    if(res == MCP_DAEMON_BUSY) return -EBUSY;
    assert(res == MCP_DAEMON_OK);

    uint8_t buf[2];

    buf[0] = 0; // protocol
    mcp_daemon_write(con, buf, 1);
    mcp_daemon_read(con, buf, 1);
    if(buf[0] != 0) { // protocol not supported
        mcp_daemon_disconnect(con);
        return -ENOENT;
    }

    buf[0] = FS_BASE_ACTION_DELETE;
    buf[1] = filename_len;
    mcp_daemon_write(con, buf, 2);
    mcp_daemon_write(con, p, filename_len);
    mcp_daemon_read(con, buf, 1);
    switch(buf[0]) {
        case 0: break;
        case 1: return -EIO;
        case 2: return -EACCES;
        case 3: return -ENOENT;
        case 4: return -ENAMETOOLONG;
        default: assert(0);
    }

    mcp_daemon_disconnect(con);

    return 0;
}

static int op_mkdir(FAR void *volinfo, FAR const char *relpath,
    mode_t mode)
{
    return -EPERM;
}

static int op_rmdir(FAR void *volinfo, FAR const char *relpath)
{
    return -EPERM;
}

static int op_rename(FAR void *volinfo, FAR const char *oldrelpath,
    FAR const char *newrelpath)
{
    return -ENOTSUP;
}

static int op_stat(FAR void *volinfo, FAR const char *relpath,
    FAR struct stat *buf)
{
    int res;

    const char * p = relpath;
    int peer_id = decode_path(&p);

    bool is_peer_dir = peer_id >= 0 && *p == '\0';
    bool is_dir = is_peer_dir || *relpath == '\0';

    if(is_dir) {
        if(is_peer_dir) {
            volinfo_t * vinfo = volinfo;

            if(peer_id == vinfo->self_index) return -ENOENT;

            peer_t * peer = peer_id < vinfo->peer_count ? &vinfo->peers[peer_id] : NULL;
            if(!peer || !peer->has_been_connected_to) {
                if(!check_peer_is_present(peer_id)) {
                    return -ENOENT;
                }
                peer = volinfo_ensure_peer(vinfo, peer_id);
                peer->has_been_connected_to = true;
            }
        }

        memset(buf, 0, sizeof(*buf));
        buf->st_mode = S_IFDIR | 0666;

        return 0;
    }

    void * openinfo;

    res = op_open(volinfo, relpath, O_RDONLY, 0, &openinfo);
    if(res) return res;

    res = op_fstat(volinfo, openinfo, buf);
    if(res) {
        int res2 = op_close(volinfo, openinfo);
        assert(!res2);
        return res;
    }

    res = op_close(volinfo, openinfo);
    assert(!res);

    return 0;
}

static int op_destroy(FAR void *volinfo)
{
    volinfo_t * vinfo = volinfo;

    assert(vinfo->open_dir_count == 0);
    for(int i = 0; i < vinfo->peer_count; i++) assert(vinfo->peers[i].con == MCP_DAEMON_CON_NULL);
    free(vinfo->peers);

    vinfo->was_destroyed = true;
    return 0;
}

static int op_fchstat(FAR void *volinfo, FAR void *openinfo,
    FAR const struct stat *buf, int flags)
{
    return -EPERM;
}

static int op_chstat(FAR void *volinfo, FAR const char *relpath,
    FAR const struct stat *buf, int flags)
{
    return -EPERM;
}

static const struct userfs_operations_s ops = {
    op_open,
    op_close,
    op_read,
    op_write,
    op_seek,
    op_ioctl,
    op_sync,
    op_dup,
    op_fstat,
    op_truncate,
    op_opendir,
    op_closedir,
    op_readdir,
    op_rewinddir,
    op_statfs,
    op_unlink,
    op_mkdir,
    op_rmdir,
    op_rename,
    op_stat,
    op_destroy,
    op_fchstat,
    op_chstat
};

int mcp_fs_main(int argc, char *argv[])
{
    volinfo_t vinfo = {
        .peer_count = 0,
        .self_index = -1,
        .peers = NULL,
        .was_destroyed = false
    };

    userfs_run("/mnt/mcp", &ops, &vinfo, 0x4000);
    assert(vinfo.was_destroyed);

    return 0;
}
