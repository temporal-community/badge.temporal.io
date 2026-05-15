#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "temporalbadge_runtime.h"

extern "C" int64_t temporalbadge_runtime_set_time(int64_t epoch)
{
    if (epoch <= 0)
        return -1;

    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(epoch);
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0)
        return -1;

    return static_cast<int64_t>(time(nullptr));
}
