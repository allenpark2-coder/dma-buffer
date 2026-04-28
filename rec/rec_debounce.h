/* rec/rec_debounce.h — Signal debouncer for Recorder trigger events */
#ifndef REC_DEBOUNCE_H
#define REC_DEBOUNCE_H

#include <stdint.h>
#include <stdbool.h>
#include "rec_defs.h"

typedef struct {
    uint64_t last_start_ns;   /* timestamp of last accepted START (0 = never) */
    uint64_t last_stop_ns;    /* timestamp of last accepted STOP  (0 = never) */
    uint32_t debounce_ms;     /* filter window in milliseconds */
} rec_debounce_t;

/*
 * rec_debounce_init(): zero timestamps, set window.
 */
void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms);

/*
 * rec_debounce_filter():
 *   Returns true  → event passes, caller should process it.
 *   Returns false → event is within the debounce window, discard.
 *
 * START rule: filtered if (ts_ns - last_start_ns) < debounce_ns.
 * STOP  rule: filtered if (ts_ns - last_start_ns) < debounce_ns
 *             (prevents glitch STOP that arrives right after a START).
 *
 * On pass, updates last_start_ns or last_stop_ns.
 */
bool rec_debounce_filter(rec_debounce_t *d, rec_trigger_type_t type,
                         uint64_t ts_ns);

#endif /* REC_DEBOUNCE_H */
