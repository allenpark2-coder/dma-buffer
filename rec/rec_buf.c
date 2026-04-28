/* rec/rec_buf.c — Circular Buffer + I-Frame Indexer (Phase R1) */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rec_buf.h"

/* ─── Internal span type for overlap detection (§7.4) ───────────── */
typedef struct {
    uint32_t off;
    uint32_t size;
} rec_span_t;

/*
 * rec_buf_split_span(): Split a potentially-wrapping span into 1 or 2
 * linear segments within [0, ring_size).
 * Returns number of valid spans (1 or 2).
 */
static int rec_buf_split_span(uint32_t off, uint32_t size, uint32_t ring_size,
                               rec_span_t spans[2])
{
    if (size == 0) {
        spans[0].off  = off;
        spans[0].size = 0;
        return 0;
    }

    uint32_t end = off + size; /* may exceed ring_size — that's the wrap */
    if (end <= ring_size) {
        /* no wrap */
        spans[0].off  = off;
        spans[0].size = size;
        return 1;
    } else {
        /* wraps around */
        spans[0].off  = off;
        spans[0].size = ring_size - off;   /* first chunk: to end of ring */
        spans[1].off  = 0;
        spans[1].size = size - spans[0].size; /* second chunk: from start */
        return 2;
    }
}

/*
 * rec_linear_overlap(): Check if two linear (non-wrapping) spans overlap.
 */
static bool rec_linear_overlap(rec_span_t a, rec_span_t b)
{
    if (a.size == 0 || b.size == 0)
        return false;
    /* [a.off, a.off+a.size) overlaps [b.off, b.off+b.size) */
    return (a.off < b.off + b.size) && (b.off < a.off + a.size);
}

/*
 * rec_buf_overlaps(): 2x2 overlap check for two potentially-wrapping spans
 * in a ring of ring_size bytes.
 */
static bool rec_buf_overlaps(uint32_t a_off, uint32_t a_size,
                              uint32_t b_off, uint32_t b_size,
                              uint32_t ring_size)
{
    rec_span_t a_spans[2], b_spans[2];
    int na = rec_buf_split_span(a_off, a_size, ring_size, a_spans);
    int nb = rec_buf_split_span(b_off, b_size, ring_size, b_spans);

    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (rec_linear_overlap(a_spans[i], b_spans[j]))
                return true;
        }
    }
    return false;
}

/* ─── Public overlap checker (exposed for unit tests via extern) ─── */
bool rec_buf_overlaps_pub(uint32_t a_off, uint32_t a_size,
                           uint32_t b_off, uint32_t b_size,
                           uint32_t ring_size)
{
    return rec_buf_overlaps(a_off, a_size, b_off, b_size, ring_size);
}

/* ─── Monotonic microseconds helper ─────────────────────────────── */
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ─── rec_buf_create / destroy ───────────────────────────────────── */
rec_buf_t *rec_buf_create(uint32_t ring_size)
{
    if (ring_size < REC_BUF_SIZE_MIN || ring_size > REC_BUF_SIZE_MAX)
        return NULL;

    rec_buf_t *buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->ring = malloc(ring_size);
    if (!buf->ring) {
        free(buf);
        return NULL;
    }

    buf->ring_size  = ring_size;
    buf->write_pos  = 0;
    buf->index_head = 0;
    buf->index_tail = 0;
    buf->index_count = 0;

    atomic_store_explicit(&buf->protected_read_offset, REC_PROTECT_NONE,
                          memory_order_relaxed);
    atomic_store_explicit(&buf->protected_read_size, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&buf->protected_gen, REC_PRE_GEN_NONE,
                          memory_order_relaxed);
    atomic_store_explicit(&buf->next_pre_gen, 0, memory_order_relaxed);
    atomic_store_explicit(&buf->aborted_pre_gen, REC_PRE_GEN_NONE,
                          memory_order_relaxed);

    atomic_store_explicit(&buf->pre_queue.head, 0, memory_order_relaxed);
    atomic_store_explicit(&buf->pre_queue.tail, 0, memory_order_relaxed);

    return buf;
}

void rec_buf_destroy(rec_buf_t **buf)
{
    if (!buf || !*buf)
        return;
    free((*buf)->ring);
    free(*buf);
    *buf = NULL;
}

/* ─── rec_buf_push ───────────────────────────────────────────────── */
int rec_buf_push(rec_buf_t *buf,
                 const uint8_t *data, uint32_t size,
                 uint64_t timestamp_ns, uint64_t seq_num,
                 bool is_keyframe)
{
    if (!buf || !data || size == 0)
        return REC_ERR_QUEUE_FULL;

    if (size > buf->ring_size)
        return REC_ERR_WRITER_STUCK; /* frame too large */

    /* ── Check protect overlap ───────────────────────────────────── */
    uint32_t prot_gen = atomic_load_explicit(&buf->protected_gen,
                                              memory_order_acquire);
    if (prot_gen != REC_PRE_GEN_NONE) {
        uint32_t prot_off  = atomic_load_explicit(&buf->protected_read_offset,
                                                   memory_order_acquire);
        uint32_t prot_size = atomic_load_explicit(&buf->protected_read_size,
                                                   memory_order_acquire);

        if (prot_off != REC_PROTECT_NONE && prot_size > 0 &&
            rec_buf_overlaps(buf->write_pos, size,
                              prot_off, prot_size,
                              buf->ring_size))
        {
            /* Spin-wait up to REC_PROTECT_SPIN_TIMEOUT_US */
            uint64_t deadline = now_us() + REC_PROTECT_SPIN_TIMEOUT_US;
            bool overlap = true;
            while (now_us() < deadline) {
                prot_gen = atomic_load_explicit(&buf->protected_gen,
                                                 memory_order_acquire);
                if (prot_gen == REC_PRE_GEN_NONE) {
                    overlap = false;
                    break;
                }
                prot_off  = atomic_load_explicit(&buf->protected_read_offset,
                                                  memory_order_acquire);
                prot_size = atomic_load_explicit(&buf->protected_read_size,
                                                  memory_order_acquire);
                if (prot_off == REC_PROTECT_NONE || prot_size == 0 ||
                    !rec_buf_overlaps(buf->write_pos, size,
                                       prot_off, prot_size,
                                       buf->ring_size))
                {
                    overlap = false;
                    break;
                }
            }

            if (overlap) {
                /* Timeout: abort the batch and clear protect */
                uint32_t cur_gen = atomic_load_explicit(&buf->protected_gen,
                                                         memory_order_acquire);
                atomic_store_explicit(&buf->aborted_pre_gen, cur_gen,
                                       memory_order_release);
                atomic_store_explicit(&buf->protected_gen, REC_PRE_GEN_NONE,
                                       memory_order_release);
                atomic_store_explicit(&buf->protected_read_offset,
                                       REC_PROTECT_NONE, memory_order_release);
                atomic_store_explicit(&buf->protected_read_size, 0,
                                       memory_order_release);
                return REC_ERR_WRITER_STUCK;
            }
        }
    }

    /* ── Evict index entries whose ring data would be overwritten ── */
    uint32_t wp   = buf->write_pos;
    uint32_t end  = (wp + size) % buf->ring_size;
    bool     wrap = (wp + size) > buf->ring_size;

    /* Evict oldest entries that overlap with the region we're about to write */
    while (buf->index_count > 0) {
        rec_frame_entry_t *oldest = &buf->index[buf->index_head];
        if (rec_buf_overlaps(wp, size,
                              oldest->offset, oldest->size,
                              buf->ring_size))
        {
            buf->index_head = (buf->index_head + 1) % REC_FRAME_INDEX_MAX;
            buf->index_count--;
        } else {
            break;
        }
    }
    (void)end;
    (void)wrap;

    /* ── Write frame data to ring (handle wrap) ──────────────────── */
    if (buf->write_pos + size <= buf->ring_size) {
        memcpy(buf->ring + buf->write_pos, data, size);
    } else {
        uint32_t first_chunk = buf->ring_size - buf->write_pos;
        memcpy(buf->ring + buf->write_pos, data, first_chunk);
        memcpy(buf->ring, data + first_chunk, size - first_chunk);
    }

    /* ── Add entry to index ──────────────────────────────────────── */
    if (buf->index_count == REC_FRAME_INDEX_MAX) {
        /* Index full: evict oldest */
        buf->index_head = (buf->index_head + 1) % REC_FRAME_INDEX_MAX;
        buf->index_count--;
    }

    rec_frame_entry_t *entry = &buf->index[buf->index_tail];
    entry->offset       = buf->write_pos;
    entry->size         = size;
    entry->timestamp_ns = timestamp_ns;
    entry->seq_num      = seq_num;
    entry->batch_gen    = 0;
    entry->is_keyframe  = is_keyframe;

    buf->index_tail  = (buf->index_tail + 1) % REC_FRAME_INDEX_MAX;
    buf->index_count++;

    /* ── Advance write position ──────────────────────────────────── */
    buf->write_pos = (buf->write_pos + size) % buf->ring_size;

    return REC_OK;
}

/* ─── rec_buf_extract_from_keyframe ──────────────────────────────── */
int rec_buf_extract_from_keyframe(rec_buf_t *buf,
                                   uint64_t   now_ns,
                                   uint32_t   pre_sec,
                                   uint32_t  *batch_gen_out)
{
    if (!buf || !batch_gen_out)
        return 0;

    if (buf->index_count == 0)
        return 0;

    uint64_t target_ns = now_ns - (uint64_t)pre_sec * 1000000000ULL;

    /*
     * Scan from oldest to newest.
     * Find the last keyframe with timestamp_ns <= target_ns.
     * If none found, fall back to the oldest keyframe.
     */
    int32_t  best_idx       = -1;   /* index into circular index array */
    int32_t  oldest_kf_idx  = -1;
    uint64_t best_ts        = 0;

    for (uint32_t i = 0; i < buf->index_count; i++) {
        uint32_t idx = (buf->index_head + i) % REC_FRAME_INDEX_MAX;
        rec_frame_entry_t *e = &buf->index[idx];

        if (!e->is_keyframe)
            continue;

        if (oldest_kf_idx < 0)
            oldest_kf_idx = (int32_t)idx;

        if (e->timestamp_ns <= target_ns) {
            if (best_idx < 0 || e->timestamp_ns >= best_ts) {
                best_idx = (int32_t)idx;
                best_ts  = e->timestamp_ns;
            }
        }
    }

    int32_t start_idx = (best_idx >= 0) ? best_idx : oldest_kf_idx;
    if (start_idx < 0)
        return 0;   /* no keyframe at all */

    /* ── Allocate batch gen ──────────────────────────────────────── */
    uint32_t pre_gen = atomic_fetch_add_explicit(&buf->next_pre_gen, 1,
                                                  memory_order_relaxed) + 1;

    /* NOTE: aborted_pre_gen is cleared AFTER enqueuing new entries (below).
     * Clearing it here would let the writer skip the abort check on stale
     * entries that may still be in the pre_queue from the previous batch. */

    /* ── Compute protect window: from start_idx.offset to write_pos ─ */
    uint32_t prot_off  = buf->index[(uint32_t)start_idx].offset;
    uint32_t prot_end  = buf->write_pos;
    uint32_t prot_size;

    if (prot_end > prot_off) {
        prot_size = prot_end - prot_off;
    } else if (prot_end == prot_off) {
        /* protect entire ring */
        prot_size = buf->ring_size;
    } else {
        /* wraps: from prot_off to end of ring + 0 to prot_end */
        prot_size = (buf->ring_size - prot_off) + prot_end;
    }

    /* Set protect window (order: size, offset, gen — all release) */
    atomic_store_explicit(&buf->protected_read_size,   prot_size,
                           memory_order_release);
    atomic_store_explicit(&buf->protected_read_offset, prot_off,
                           memory_order_release);
    atomic_store_explicit(&buf->protected_gen,         pre_gen,
                           memory_order_release);

    /* ── Enqueue entries from start_idx to index_tail into pre_queue ─ */
    /* Find the position of start_idx within the circular buffer */
    uint32_t start_pos = 0;
    for (uint32_t i = 0; i < buf->index_count; i++) {
        if ((buf->index_head + i) % REC_FRAME_INDEX_MAX == (uint32_t)start_idx) {
            start_pos = i;
            break;
        }
    }

    int count = 0;
    for (uint32_t i = start_pos; i < buf->index_count; i++) {
        uint32_t idx = (buf->index_head + i) % REC_FRAME_INDEX_MAX;

        uint32_t qtail = atomic_load_explicit(&buf->pre_queue.tail,
                                               memory_order_relaxed);
        uint32_t qhead = atomic_load_explicit(&buf->pre_queue.head,
                                               memory_order_acquire);

        uint32_t next_tail = (qtail + 1) % REC_PRE_QUEUE_DEPTH;
        if (next_tail == qhead)
            break;  /* queue full */

        rec_frame_entry_t entry = buf->index[idx];
        entry.batch_gen = pre_gen;
        buf->pre_queue.entries[qtail] = entry;
        atomic_store_explicit(&buf->pre_queue.tail, next_tail,
                               memory_order_release);
        count++;
    }

    /* All new entries are tagged with pre_gen; old aborted entries remain
     * identifiable by their batch_gen until we clear the abort sentinel here.
     * The writer will have snapshot-ed aborted_pre_gen at drain-loop start,
     * so stale entries are correctly skipped even after this store. */
    atomic_store_explicit(&buf->aborted_pre_gen, REC_PRE_GEN_NONE,
                           memory_order_release);

    *batch_gen_out = pre_gen;
    return count;
}

/* ─── rec_buf_abort_pre ──────────────────────────────────────────── */
void rec_buf_abort_pre(rec_buf_t *buf, uint32_t batch_gen)
{
    if (!buf)
        return;

    uint32_t cur_gen = atomic_load_explicit(&buf->protected_gen,
                                             memory_order_acquire);
    if (cur_gen != batch_gen)
        return;  /* old batch cannot clear new batch's protect */

    atomic_store_explicit(&buf->aborted_pre_gen, batch_gen,
                           memory_order_release);
    atomic_store_explicit(&buf->protected_gen, REC_PRE_GEN_NONE,
                           memory_order_release);
    atomic_store_explicit(&buf->protected_read_offset, REC_PROTECT_NONE,
                           memory_order_release);
    atomic_store_explicit(&buf->protected_read_size, 0,
                           memory_order_release);
}

/* ─── rec_pre_queue_dequeue ──────────────────────────────────────── */
bool rec_pre_queue_dequeue(rec_pre_queue_t *q, rec_frame_entry_t *out)
{
    if (!q || !out)
        return false;

    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail)
        return false;  /* empty */

    *out = q->entries[head];
    atomic_store_explicit(&q->head, (head + 1) % REC_PRE_QUEUE_DEPTH,
                           memory_order_release);
    return true;
}

/* ─── rec_buf_read_ring ──────────────────────────────────────────── */
void rec_buf_read_ring(const rec_buf_t *buf, uint32_t offset, uint32_t size,
                        uint8_t *dst)
{
    if (!buf || !dst || size == 0)
        return;

    if (offset + size <= buf->ring_size) {
        memcpy(dst, buf->ring + offset, size);
    } else {
        uint32_t first_chunk = buf->ring_size - offset;
        memcpy(dst, buf->ring + offset, first_chunk);
        memcpy(dst + first_chunk, buf->ring, size - first_chunk);
    }
}
