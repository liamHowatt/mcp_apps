#pragma once

#include "beeper_task.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
