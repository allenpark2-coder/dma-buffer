/* rec/rec_debounce.c */
#include <string.h>
#include "rec_debounce.h"

void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms)
{
    memset(d, 0, sizeof(*d));
    d->debounce_ms = debounce_ms;
}

bool rec_debounce_filter(rec_debounce_t *d, rec_trigger_type_t type,
                         uint64_t ts_ns)
{
    uint64_t window_ns = (uint64_t)d->debounce_ms * 1000000ULL;

    if (type == REC_TRIGGER_START) {
        if (d->last_start_ns != 0 &&
            ts_ns - d->last_start_ns < window_ns)
            return false;   /* filtered */
        d->last_start_ns = ts_ns;
        return true;
    } else {
        /* TRIGGER_STOP: discard if too close to last accepted START */
        if (d->last_start_ns != 0 &&
            ts_ns - d->last_start_ns < window_ns)
            return false;
        return true;
    }
}
