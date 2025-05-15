#include "beeper_private.h"

char * beeper_read_text_file(const char * path)
{
    int fd = open(path, O_RDONLY);
    if(fd == -1) {
        int en = errno;
        assert(en == ENOENT
               || en == EBADF); /* host_fs sets this instead */
        return NULL;
    }
    struct stat statbuf;
    int res = fstat(fd, &statbuf);
    assert(res == 0);
    ssize_t sz = statbuf.st_size;
    char * buf = malloc(sz + 1);
    assert(buf);
    buf[sz] = '\0';
    ssize_t br = read(fd, buf, sz);
    assert(br == sz);
    res = close(fd);
    assert(res == 0);
    return buf;
}

void beeper_queue_init(beeper_queue_t * bq, size_t item_size)
{
    bq->item_size = item_size;
    bq->len = 0;
    bq->cap = 0;
    bq->data = NULL;
}

void beeper_queue_destroy(beeper_queue_t * bq)
{
    free(bq->data);
}

void beeper_queue_push(beeper_queue_t * bq, const void * to_push)
{
    if(bq->len == bq->cap) {
        if(bq->cap == 0) bq->cap = 2;
        else bq->cap *= 2;
        bq->data = realloc(bq->data, bq->cap * bq->item_size);
        assert(bq->data);
    }
    memcpy(bq->data + bq->len * bq->item_size, to_push, bq->item_size);
    bq->len += 1;
}

bool beeper_queue_pop(beeper_queue_t * bq, void * pop_dst)
{
    if(bq->len == 0) return false;
    bq->len -= 1;
    memcpy(pop_dst, bq->data, bq->item_size);
    memmove(bq->data, bq->data + bq->item_size, bq->len * bq->item_size);
    if(bq->len == 0) {
        free(bq->data);
        bq->data = NULL;
        bq->cap = 0;
    }
    return true;
}

bool beeper_queue_is_empty(beeper_queue_t * bq)
{
    return bq->len == 0;
}
