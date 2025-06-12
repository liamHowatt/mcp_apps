#include "beeper_private.h"

#include <sys/eventfd.h>

void * beeper_asserting_calloc(size_t nmemb, size_t size)
{
    void * ret = calloc(nmemb, size);
    assert(ret);
    return ret;
}

void * beeper_asserting_malloc(size_t size)
{
    void * ret = malloc(size);
    assert(ret);
    return ret;
}

void * beeper_asserting_realloc(void * ptr, size_t size)
{
    void * ret = realloc(ptr, size);
    assert(ret);
    return ret;
}

char * beeper_asserting_strdup(const char * s)
{
    char * new_s = strdup(s);
    assert(new_s);
    return new_s;
}

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

void beeper_array_init(beeper_array_t * ba, size_t item_size)
{
    ba->item_size = item_size;
    ba->len = 0;
    ba->cap = 0;
    ba->data = NULL;
}

void beeper_array_destroy(beeper_array_t * ba)
{
    free(ba->data);
}

size_t beeper_array_len(beeper_array_t * ba)
{
    return ba->len;
}

void * beeper_array_data(beeper_array_t * ba)
{
    return ba->data;
}

void beeper_array_append(beeper_array_t * ba, const void * to_append)
{
    if(ba->len == ba->cap) {
        if(ba->cap == 0) ba->cap = 2;
        else ba->cap += ba->cap / 2;
        ba->data = realloc(ba->data, ba->cap * ba->item_size);
        assert(ba->data);
    }
    if(to_append) {
        memcpy(ba->data + ba->len * ba->item_size, to_append, ba->item_size);
    }
    ba->len += 1;
}

void beeper_array_remove(beeper_array_t * ba, size_t index)
{
    assert(index < ba->len);
    ba->len -= 1;
    if(index != ba->len) {
        size_t offset = index * ba->item_size;
        memmove(ba->data + offset, ba->data + (offset + ba->item_size), (ba->len - index) * ba->item_size);
    }
}

void beeper_array_reset(beeper_array_t * ba)
{
    ba->len = 0;
    ba->cap = 0;
    free(ba->data);
    ba->data = NULL;
}

void beeper_queue_init(beeper_queue_t * bq, size_t item_size)
{
    beeper_array_init(&bq->a, item_size);
    bq->evfd = eventfd(0, EFD_CLOEXEC);
    assert(bq->evfd >= 0);
}

void beeper_queue_destroy(beeper_queue_t * bq)
{
    beeper_array_destroy(&bq->a);
    int res = close(bq->evfd);
    assert(res == 0);
}

void beeper_queue_push(beeper_queue_t * bq, const void * to_push)
{
    if(beeper_array_len(&bq->a) == 0) {
        uint64_t notify = 1;
        ssize_t rwres = write(bq->evfd, &notify, 8);
        assert(rwres == 8);
    }
    beeper_array_append(&bq->a, to_push);
}

bool beeper_queue_pop(beeper_queue_t * bq, void * pop_dst)
{
    size_t array_len = beeper_array_len(&bq->a);
    if(array_len == 0) return false;
    void * array_data = beeper_array_data(&bq->a);
    memcpy(pop_dst, array_data, bq->a.item_size);
    if(array_len == 1) {
        beeper_array_reset(&bq->a);

        uint64_t notify;
        ssize_t rwres = read(bq->evfd, &notify, 8);
        assert(rwres == 8);
        assert(notify == 1);
    }
    else {
        beeper_array_remove(&bq->a, 0);
    }
    return true;
}

int beeper_queue_get_poll_fd(const beeper_queue_t * bq)
{
    return bq->evfd;
}

void * beeper_dict_get_create(beeper_array_t * ba, const char * key,
                              beeper_dict_cb_t create_cb, bool * was_created)
{
    size_t array_len = beeper_array_len(ba);
    size_t item_size = ba->item_size;
    uint8_t * p = beeper_array_data(ba);
    for(size_t i = 0; i < array_len; i++) {
        char * item_key = *(char **)p;
        if(0 == strcmp(key, item_key)) {
            if(was_created) *was_created = false;
            return p;
        }
        p += item_size;
    }
    beeper_array_append(ba, NULL);
    p = beeper_array_data(ba);
    p += (array_len * item_size);
    char * key_copy = strdup(key);
    assert(key_copy);
    *(char **)p = key_copy;
    if(create_cb) create_cb(p);
    if(was_created) *was_created = true;
    return p;
}

void beeper_dict_destroy(beeper_array_t * ba, beeper_dict_cb_t destroy_cb)
{
    size_t array_len = beeper_array_len(ba);
    size_t item_size = ba->item_size;
    uint8_t * p = beeper_array_data(ba);
    for(size_t i = 0; i < array_len; i++) {
        if(destroy_cb) destroy_cb(p);
        free(*(char **)p);
        p += item_size;
    }
    beeper_array_destroy(ba);
}

void beeper_dict_item_memzero(void * item, size_t item_size)
{
    memset(item + sizeof(char *), 0, item_size - sizeof(char *));
}
