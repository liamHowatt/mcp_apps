#include <stdio.h>
#include <stdlib.h>

int custom_hello_main(int argc, char *argv[])
{
    puts("custom hello");

    char * peer = getenv("MCP_PEER");
    if(peer == NULL) peer = "NULL";
    printf("peer: '%s'\n", peer);

    return 0;
}
