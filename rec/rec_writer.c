#include "rec_writer.h"
#include "rec_ts_mux.h"
#include "rec_segment.h"
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
    uint32_t tail = atomic_load_explicit(&w->write_queue.tail, memory_order_relaxed);
    uint32_t next = (tail + 1) % REC_WRITE_QUEUE_DEPTH;
    if (next == atomic_load_explicit(&w->write_queue.head, memory_order_acquire))
        return REC_ERR_QUEUE_FULL;
    w->write_queue.items[tail] = item;
    atomic_store_explicit(&w->write_queue.tail, next, memory_order_release);
    uint64_t one = 1;
    ssize_t _wr = write(w->eventfd, &one, sizeof(one));
    (void)_wr;
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

static void *writer_thread(void *arg)
{
    rec_writer_t *w = arg;
    uint64_t val;
    while (read(w->eventfd, &val, sizeof(val)) > 0) {
        uint32_t head = atomic_load_explicit(&w->write_queue.head, memory_order_relaxed);
        while (1) {
            uint32_t tail = atomic_load_explicit(&w->write_queue.tail, memory_order_acquire);
            if (head == tail) break;
            rec_write_item_t item = w->write_queue.items[head];
            head = (head + 1) % REC_WRITE_QUEUE_DEPTH;
            atomic_store_explicit(&w->write_queue.head, head, memory_order_release);
            if (item.is_shutdown_sentinel) goto done;
            free(item.data);
        }
    }
done:
    return NULL;
}
