#include "ag_retry.h"

unsigned ag_retry_next_delay(unsigned current_seconds, unsigned maximum_seconds)
{
    if (current_seconds >= maximum_seconds
        || current_seconds > maximum_seconds - current_seconds) {
        return maximum_seconds;
    }
    return current_seconds * 2u;
}
