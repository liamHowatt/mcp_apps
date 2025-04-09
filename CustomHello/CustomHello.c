#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>

#include <nuttx/input/keyboard.h>

int custom_hello_main(int argc, char *argv[])
{
    ssize_t rwres;

    int fd = open("/dev/ukeyboard", O_RDONLY);
    while(1) {
        struct keyboard_event_s keypad_event;
        rwres = read(fd, &keypad_event, sizeof(keypad_event));
        assert(rwres == sizeof(keypad_event));
        printf("%"PRIu32" %"PRIu32"\n", keypad_event.code, keypad_event.type);
    }

    return 0;
}
