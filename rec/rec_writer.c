#include "rec_writer.h"
#include "rec_ts_mux.h"
#include "rec_segment.h"
#include "rec_buf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <stdatomic.h>

struct rec_writer {
    rec_buf_t           *ring;
    rec_codec_config_t  *codec_cfg;
    rec_writer_config_t  cfg;
    rec_write_queue_t    write_queue;
    int                  eventfd;
    pthread_t            thread;
    _Atomic uint64_t     written_bytes;
    _Atomic uint32_t     dropped_frames;
    bool                 overflow_drop;
    bool                 pending_cut;
};

static void *writer_thread(void *arg);

rec_writer_t *rec_writer_create(rec_buf_t          *ring,
                                rec_codec_config_t *codec_cfg,
                                const rec_writer_config_t *cfg)
{
    rec_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->ring      = ring;
    w->codec_cfg = codec_cfg;
    w->cfg       = *cfg;
    atomic_init(&w->write_queue.head, 0);
    atomic_init(&w->write_queue.tail, 0);
    atomic_init(&w->written_bytes,  0);
    atomic_init(&w->dropped_frames, 0);

    w->eventfd = eventfd(0, EFD_CLOEXEC);
    if (w->eventfd < 0) { free(w); return NULL; }

    if (pthread_create(&w->thread, NULL, writer_thread, w) != 0) {
        close(w->eventfd);
        free(w);
        return NULL;
    }
    return w;
}

int rec_writer_get_eventfd(const rec_writer_t *w)
{
    return w ? w->eventfd : -1;
}

int rec_writer_enqueue(rec_writer_t *w, rec_write_item_t item)
{
    if (item.is_shutdown_sentinel) {
        /* Spin until a slot is free, then enqueue the sentinel.
         * The writer always has a pending eventfd signal when the queue is
         * full (the flood signals at least once), so this spin exits quickly. */
        uint32_t tail, next;
        do {
            tail = atomic_load_explicit(&w->write_queue.tail,
                                        memory_order_relaxed);
            next = (tail + 1) % REC_WRITE_QUEUE_DEPTH;
        } while (next == atomic_load_explicit(&w->write_queue.head,
                                              memory_order_acquire));

        w->write_queue.items[tail] = item;
        atomic_store_explicit(&w->write_queue.tail, next,
                              memory_order_release);
        uint64_t one = 1;
        ssize_t _wr = write(w->eventfd, &one, sizeof(one));
        (void)_wr;
        return REC_OK;
    }

    uint32_t tail = atomic_load_explicit(&w->write_queue.tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&w->write_queue.head, memory_order_acquire);
    uint32_t next = (tail + 1) % REC_WRITE_QUEUE_DEPTH;

    if (next == head) {
        /* Queue full — overflow handling */
        atomic_fetch_add_explicit(&w->dropped_frames, 1, memory_order_relaxed);
        return REC_ERR_QUEUE_FULL;
    }

    w->write_queue.items[tail] = item;
    /* seq_cst store so the post-store head reload below is ordered correctly */
    atomic_store_explicit(&w->write_queue.tail, next, memory_order_seq_cst);

    /* Signal the writer only when the queue was empty (empty→non-empty edge).
     * Reload head with seq_cst after the tail store so we cannot miss a
     * wakeup: if the writer drained to empty and is about to block, either
     * it will see our new tail (and skip the eventfd read) or it will block
     * and we will see head == tail here and send the signal. */
    uint32_t head_after = atomic_load_explicit(&w->write_queue.head,
                                               memory_order_seq_cst);
    if (head_after == tail) {
        uint64_t one = 1;
        ssize_t _wr = write(w->eventfd, &one, sizeof(one));
        (void)_wr;
    }
    return REC_OK;
}

void rec_writer_publish_codec_config(rec_writer_t *w,
                                     const uint8_t *data, uint32_t size,
                                     uint32_t format)
{
    uint32_t active   = atomic_load_explicit(&w->codec_cfg->active_slot,
                                             memory_order_acquire);
    uint32_t inactive = 1 - active;
    rec_codec_blob_t *slot = &w->codec_cfg->slots[inactive];
    if (size > REC_CODEC_CONFIG_MAX_SIZE) size = REC_CODEC_CONFIG_MAX_SIZE;
    memcpy(slot->data, data, size);
    slot->size   = size;
    slot->format = format;
    atomic_store_explicit(&w->codec_cfg->active_slot, inactive, memory_order_release);
    atomic_fetch_add_explicit(&w->codec_cfg->version, 1, memory_order_relaxed);
}

void rec_writer_destroy(rec_writer_t **wp)
{
    if (!wp || !*wp) return;
    rec_writer_t *w = *wp;
    rec_write_item_t sentinel = { .is_shutdown_sentinel = true };
    rec_writer_enqueue(w, sentinel);
    pthread_join(w->thread, NULL);
    close(w->eventfd);
    free(w);
    *wp = NULL;
}

uint64_t rec_writer_get_written_bytes(const rec_writer_t *w)
{
    return w ? atomic_load_explicit(&w->written_bytes, memory_order_relaxed) : 0;
}

uint32_t rec_writer_get_dropped_frames(const rec_writer_t *w)
{
    return w ? atomic_load_explicit(&w->dropped_frames, memory_order_relaxed) : 0;
}

static rec_segment_t *open_new_segment(rec_writer_t *w)
{
    return rec_segment_open(w->cfg.output_dir,
                            w->cfg.stream_name,
                            w->cfg.mode,
                            w->cfg.segment_duration_sec,
                            w->cfg.segment_size_max,
                            w->cfg.flush_interval_sec);
}

static void *writer_thread(void *arg)
{
    rec_writer_t  *w   = arg;
    rec_segment_t *seg = NULL;
    uint8_t cc_pat = 0, cc_pmt = 0, cc_vid = 0;
    uint64_t val;

    while (read(w->eventfd, &val, sizeof(val)) > 0) {

        /* ── [A] drain pre_extract_queue (priority) ──────────────── */
        if (w->ring) {
            uint32_t abort_snapshot = atomic_load_explicit(
                &w->ring->aborted_pre_gen, memory_order_acquire);

            rec_frame_entry_t entry;
            while (rec_pre_queue_dequeue(&w->ring->pre_queue, &entry)) {
                bool is_aborted = (entry.batch_gen != 0 &&
                                   entry.batch_gen == abort_snapshot);

                uint8_t *tmp = malloc(entry.size);
                if (!tmp) continue;

                rec_buf_read_ring(w->ring, entry.offset, entry.size, tmp);

                /* Check abort again after copy */
                is_aborted |= (entry.batch_gen != 0 &&
                               entry.batch_gen == atomic_load_explicit(
                                   &w->ring->aborted_pre_gen, memory_order_acquire));

                if (!is_aborted && seg) {
                    size_t bsz = REC_TS_PES_BUF_SIZE(entry.size);
                    uint8_t *tb = malloc(bsz);
                    if (tb) {
                        int n = rec_ts_write_pes(tb, bsz, tmp, entry.size,
                                                 entry.timestamp_ns,
                                                 entry.timestamp_ns,
                                                 entry.is_keyframe, &cc_vid);
                        if (n > 0) rec_segment_write(seg, tb, n, 33333333ull);
                        free(tb);
                    }
                }
                free(tmp);
            }

            /* Clear protect window after draining */
            uint32_t own_gen = atomic_load_explicit(&w->ring->protected_gen,
                                                    memory_order_acquire);
            if (own_gen != 0 && own_gen != REC_PRE_GEN_NONE) {
                atomic_store_explicit(&w->ring->protected_read_size, 0,
                                      memory_order_release);
                atomic_store_explicit(&w->ring->protected_read_offset,
                                      REC_PROTECT_NONE, memory_order_release);
                atomic_store_explicit(&w->ring->protected_gen,
                                      REC_PRE_GEN_NONE, memory_order_release);
            }
        }

        /* ── [B] drain write queue ───────────────────────────────── */
        while (1) {
            uint32_t head = atomic_load_explicit(&w->write_queue.head,
                                                 memory_order_relaxed);
            uint32_t tail = atomic_load_explicit(&w->write_queue.tail,
                                                 memory_order_acquire);
            if (head == tail) break;

            rec_write_item_t item = w->write_queue.items[head];
            uint32_t next_head = (head + 1) % REC_WRITE_QUEUE_DEPTH;
            atomic_store_explicit(&w->write_queue.head, next_head,
                                  memory_order_release);

            if (item.is_shutdown_sentinel) {
                if (seg) { rec_segment_close(seg); seg = NULL; }
                goto done;
            }

            /* Open new segment if needed */
            if (!seg || item.is_segment_boundary) {
                if (seg) { rec_segment_close(seg); seg = NULL; }

                /* Only open at a keyframe with codec config available */
                uint32_t active = atomic_load_explicit(
                    &w->codec_cfg->active_slot, memory_order_acquire);
                if (w->codec_cfg->slots[active].size == 0 || !item.is_keyframe) {
                    free(item.data);
                    continue;
                }

                seg = open_new_segment(w);
                if (!seg) { free(item.data); continue; }
                cc_pat = 0; cc_pmt = 0; cc_vid = 0;

                /* PAT */
                uint8_t pat[188];
                int n = rec_ts_write_pat(pat, sizeof(pat), &cc_pat);
                if (n > 0) rec_segment_write(seg, pat, n, 0);

                /* PMT */
                uint8_t pmt[188];
                rec_codec_blob_t snap = w->codec_cfg->slots[
                    atomic_load_explicit(&w->codec_cfg->active_slot,
                                         memory_order_acquire)];
                n = rec_ts_write_pmt(pmt, sizeof(pmt), snap.format, &cc_pmt);
                if (n > 0) rec_segment_write(seg, pmt, n, 0);

                /* Codec config NALUs (SPS/PPS) */
                if (snap.size > 0) {
                    size_t bsz = REC_TS_PES_BUF_SIZE(snap.size);
                    uint8_t *tb = malloc(bsz);
                    if (tb) {
                        n = rec_ts_write_pes(tb, bsz, snap.data, snap.size,
                                             item.timestamp_ns, item.timestamp_ns,
                                             false, &cc_vid);
                        if (n > 0) rec_segment_write(seg, tb, n, 0);
                        free(tb);
                    }
                }
            }

            /* Handle pending_cut: defer to next IDR */
            if (w->pending_cut) {
                if (item.is_keyframe) {
                    w->pending_cut = false;
                    if (seg) { rec_segment_close(seg); seg = NULL; }
                    seg = open_new_segment(w);
                    if (!seg) { free(item.data); continue; }
                    cc_pat = 0; cc_pmt = 0; cc_vid = 0;

                    uint8_t pat[188];
                    int n = rec_ts_write_pat(pat, sizeof(pat), &cc_pat);
                    if (n > 0) rec_segment_write(seg, pat, n, 0);

                    uint8_t pmt[188];
                    rec_codec_blob_t snap2 = w->codec_cfg->slots[
                        atomic_load_explicit(&w->codec_cfg->active_slot,
                                             memory_order_acquire)];
                    n = rec_ts_write_pmt(pmt, sizeof(pmt), snap2.format, &cc_pmt);
                    if (n > 0) rec_segment_write(seg, pmt, n, 0);

                    if (snap2.size > 0) {
                        size_t bsz2 = REC_TS_PES_BUF_SIZE(snap2.size);
                        uint8_t *tb2 = malloc(bsz2);
                        if (tb2) {
                            n = rec_ts_write_pes(tb2, bsz2, snap2.data, snap2.size,
                                                 item.timestamp_ns, item.timestamp_ns,
                                                 false, &cc_vid);
                            if (n > 0) rec_segment_write(seg, tb2, n, 0);
                            free(tb2);
                        }
                    }
                }
            }

            if (!seg) { free(item.data); continue; }

            /* Write frame as TS packets */
            size_t bsz = REC_TS_PES_BUF_SIZE(item.size);
            uint8_t *tb = malloc(bsz);
            if (tb) {
                int n = rec_ts_write_pes(tb, bsz, item.data, item.size,
                                         item.timestamp_ns, item.timestamp_ns,
                                         item.is_keyframe, &cc_vid);
                if (n > 0) {
                    int cut = rec_segment_write(seg, tb, n, 33333333ull);
                    atomic_fetch_add_explicit(&w->written_bytes, n,
                                             memory_order_relaxed);
                    if (cut) w->pending_cut = true;
                }
                free(tb);
            }
            free(item.data);
        }
    }
done:
    if (seg) rec_segment_close(seg);
    return NULL;
}
