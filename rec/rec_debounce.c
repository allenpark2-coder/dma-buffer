/* rec/rec_debounce.c — 訊號消抖器（§5.3）*/
#include "rec_debounce.h"

void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms)
{
    d->last_start_ns = 0;
    d->debounce_ns   = (uint64_t)debounce_ms * 1000000ULL;
}

bool rec_debounce_allow_start(rec_debounce_t *d, uint64_t now_ns)
{
    /* last_start_ns == 0 表示從未觸發，永遠允許第一次 */
    if (d->last_start_ns != 0 &&
        now_ns - d->last_start_ns < d->debounce_ns)
        return false;

    d->last_start_ns = now_ns;
    return true;
}

bool rec_debounce_allow_stop(rec_debounce_t *d, uint64_t now_ns)
{
    /* STOP 在 debounce window 內到來，視為毛刺，忽略 */
    if (d->last_start_ns != 0 &&
        now_ns - d->last_start_ns < d->debounce_ns)
        return false;

    /* STOP 不更新 last_start_ns */
    return true;
}
