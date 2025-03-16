#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

static void *thr_func(void * arg)
{
    int *socp = arg;
    int soc = *socp;

    ssize_t rwres;

    char buf[10];

    rwres = read(soc, buf, 10);

    printf("actually read %ld bytes\n", (long) rwres);

    return NULL;
}

int custom_hello_main(int argc, char *argv[])
{
    puts("custom hello");

    int res;
    ssize_t rwres;

    int socs[2];
    res = socketpair(AF_UNIX, SOCK_STREAM, 0, socs);
    assert(res == 0);

    pthread_t thr;
    res = pthread_create(&thr, NULL, thr_func, &socs[1]);
    assert(res == 0);

    sleep(3);

    rwres = write(socs[0], "01234", 5);
    assert(rwres == 5);

    sleep(5);

    return 0;
}
