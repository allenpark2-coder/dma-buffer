#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include "rec_writer.h"

struct rec_writer {
    rec_buf_t            *buf;
    rec_segment_t        *segment;
    rec_write_queue_t     queue;
    rec_codec_config_t    codec_config;
    pthread_t             thread;
    int                   eventfd;
    bool                  overflow_drop;
    bool                  running;
    bool                  thread_started;
    _Atomic uint64_t      written_bytes;
    _Atomic uint32_t      dropped_frames;
};

static bool rec_write_queue_empty(const rec_write_queue_t *q)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head == tail;
}

static bool rec_write_queue_full(const rec_write_queue_t *q)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return tail - head >= REC_WRITE_QUEUE_DEPTH;
}

static bool rec_write_queue_try_enqueue(rec_write_queue_t *q, rec_write_item_t *item)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail - head >= REC_WRITE_QUEUE_DEPTH)
        return false;

    q->items[tail % REC_WRITE_QUEUE_DEPTH] = *item;
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return true;
}

static bool rec_write_queue_dequeue(rec_write_queue_t *q, rec_write_item_t *out)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head == tail)
        return false;

    *out = q->items[head % REC_WRITE_QUEUE_DEPTH];
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return true;
}

void rec_codec_config_init(rec_codec_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
}

int rec_codec_config_publish(rec_codec_config_t *cfg,
                             uint32_t            format,
                             const uint8_t      *data,
                             uint32_t            size)
{
    if (!cfg || !data || size == 0 || size > REC_CODEC_CONFIG_MAX_SIZE)
        return -EINVAL;

    uint32_t current = atomic_load_explicit(&cfg->active_slot, memory_order_relaxed);
    uint32_t next = current ^ 1u;

    memcpy(cfg->slots[next].data, data, size);
    cfg->slots[next].size = size;
    cfg->slots[next].format = format;

    atomic_store_explicit(&cfg->active_slot, next, memory_order_release);
    atomic_fetch_add_explicit(&cfg->version, 1u, memory_order_release);
    return 0;
}

bool rec_codec_config_snapshot(const rec_codec_config_t *cfg,
                               rec_codec_blob_t         *out,
                               uint32_t                 *out_version)
{
    if (!cfg || !out)
        return false;

    for (int retry = 0; retry < 3; ++retry) {
        uint32_t version_before = atomic_load_explicit(&cfg->version, memory_order_acquire);
        uint32_t slot = atomic_load_explicit(&cfg->active_slot, memory_order_acquire);
        *out = cfg->slots[slot & 1u];
        uint32_t version_after = atomic_load_explicit(&cfg->version, memory_order_acquire);
        uint32_t slot_after = atomic_load_explicit(&cfg->active_slot, memory_order_acquire);

        if (version_before == version_after && slot == slot_after) {
            if (out_version)
                *out_version = version_after;
            return out->size > 0;
        }
    }

    return false;
}

static int rec_writer_signal(rec_writer_t *writer)
{
    uint64_t one = 1;
    return write(writer->eventfd, &one, sizeof(one)) == (ssize_t)sizeof(one) ? 0 : -errno;
}

static int rec_writer_copy_ring_entry(const rec_writer_t *writer,
                                      const rec_frame_entry_t *entry,
                                      uint8_t **out)
{
    uint8_t *copy = malloc(entry->size);
    if (!copy)
        return -ENOMEM;

    if (entry->offset + entry->size <= writer->buf->ring_size) {
        memcpy(copy, writer->buf->ring + entry->offset, entry->size);
    } else {
        uint32_t first = writer->buf->ring_size - entry->offset;
        memcpy(copy, writer->buf->ring + entry->offset, first);
        memcpy(copy + first, writer->buf->ring, entry->size - first);
    }

    *out = copy;
    return 0;
}

static void rec_writer_maybe_update_written_bytes(rec_writer_t *writer)
{
    atomic_store_explicit(&writer->written_bytes,
                          rec_segment_get_written_bytes(writer->segment),
                          memory_order_release);
}

static int rec_writer_write_one(rec_writer_t       *writer,
                                const uint8_t      *data,
                                uint32_t            size,
                                uint64_t            timestamp_ns,
                                bool                is_keyframe,
                                bool                is_segment_boundary)
{
    rec_codec_blob_t codec;
    uint32_t version = 0;

    if (!rec_codec_config_snapshot(&writer->codec_config, &codec, &version)) {
        VFR_LOGW("[rec_writer] codec config unavailable, dropping frame");
        return -EAGAIN;
    }

    (void)version;
    int rc = rec_segment_write_access_unit(writer->segment,
                                           data,
                                           size,
                                           timestamp_ns,
                                           is_keyframe,
                                           is_segment_boundary,
                                           codec.format,
                                           codec.data,
                                           codec.size);
    if (rc == 0)
        rec_writer_maybe_update_written_bytes(writer);
    return rc;
}

static bool rec_writer_peek_pre_batch(const rec_pre_queue_t *q, uint32_t *batch_gen)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head == tail)
        return false;

    *batch_gen = q->entries[head % REC_PRE_QUEUE_DEPTH].batch_gen;
    return true;
}

static void rec_writer_drain_pre_queue(rec_writer_t *writer)
{
    rec_frame_entry_t entry;
    if (!rec_pre_queue_dequeue(&writer->buf->pre_queue, &entry))
        return;

    for (;;) {
        uint32_t batch_gen = entry.batch_gen;
        uint32_t abort_snapshot = batch_gen == REC_PRE_GEN_NONE
            ? REC_PRE_GEN_NONE
            : atomic_load_explicit(&writer->buf->aborted_pre_gen, memory_order_acquire);

        for (;;) {
            if (batch_gen == REC_PRE_GEN_NONE || abort_snapshot != batch_gen) {
                uint8_t *copy = NULL;
                if (rec_writer_copy_ring_entry(writer, &entry, &copy) == 0) {
                    (void)rec_writer_write_one(writer,
                                               copy,
                                               entry.size,
                                               entry.timestamp_ns,
                                               entry.is_keyframe,
                                               false);
                    free(copy);
                }
            }

            rec_frame_entry_t next;
            if (!rec_pre_queue_dequeue(&writer->buf->pre_queue, &next)) {
                if (batch_gen != REC_PRE_GEN_NONE)
                    rec_buf_clear_protect(writer->buf, batch_gen);
                return;
            }

            if (next.batch_gen != batch_gen) {
                if (batch_gen != REC_PRE_GEN_NONE)
                    rec_buf_clear_protect(writer->buf, batch_gen);
                entry = next;
                break;
            }

            entry = next;
        }
    }
}

static void rec_writer_drain_live_queue(rec_writer_t *writer)
{
    rec_write_item_t item;

    while (rec_write_queue_dequeue(&writer->queue, &item)) {
        if (item.is_shutdown_sentinel) {
            writer->running = false;
            return;
        }

        if (item.data) {
            (void)rec_writer_write_one(writer,
                                       item.data,
                                       item.size,
                                       item.timestamp_ns,
                                       item.is_keyframe,
                                       item.is_segment_boundary);
            free(item.data);
        }
    }
}

static void *rec_writer_thread_main(void *arg)
{
    rec_writer_t *writer = arg;

    while (writer->running) {
        uint64_t wake_count = 0;
        ssize_t n = read(writer->eventfd, &wake_count, sizeof(wake_count));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        rec_writer_drain_pre_queue(writer);
        rec_writer_drain_live_queue(writer);
    }

    (void)rec_segment_close(writer->segment);
    rec_writer_maybe_update_written_bytes(writer);
    return NULL;
}

rec_writer_t *rec_writer_create(const rec_writer_config_t *cfg, rec_buf_t *buf)
{
    if (!cfg || !buf || cfg->output_dir[0] == '\0' || cfg->stream_name[0] == '\0')
        return NULL;

    rec_writer_t *writer = calloc(1, sizeof(*writer));
    if (!writer)
        return NULL;

    rec_segment_config_t seg_cfg;
    memset(&seg_cfg, 0, sizeof(seg_cfg));
    snprintf(seg_cfg.output_dir, sizeof(seg_cfg.output_dir), "%s", cfg->output_dir);
    snprintf(seg_cfg.stream_name, sizeof(seg_cfg.stream_name), "%s", cfg->stream_name);
    seg_cfg.mode = cfg->mode;
    seg_cfg.duration_sec = cfg->segment_duration_sec;
    seg_cfg.size_max = cfg->segment_size_max;
    seg_cfg.flush_interval_sec = cfg->flush_interval_sec;

    writer->buf = buf;
    writer->segment = rec_segment_create(&seg_cfg);
    writer->eventfd = eventfd(0, EFD_CLOEXEC);
    writer->running = true;
    rec_codec_config_init(&writer->codec_config);

    if (!writer->segment || writer->eventfd < 0) {
        rec_writer_destroy(&writer);
        return NULL;
    }

    if (pthread_create(&writer->thread, NULL, rec_writer_thread_main, writer) != 0) {
        rec_writer_destroy(&writer);
        return NULL;
    }

    writer->thread_started = true;
    return writer;
}

void rec_writer_destroy(rec_writer_t **writer)
{
    if (!writer || !*writer)
        return;

    rec_writer_t *w = *writer;

    if (w->thread_started) {
        if (w->running) {
            rec_write_item_t sentinel;
            memset(&sentinel, 0, sizeof(sentinel));
            sentinel.is_shutdown_sentinel = true;

            while (!rec_write_queue_try_enqueue(&w->queue, &sentinel)) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
                nanosleep(&ts, NULL);
            }

            (void)rec_writer_signal(w);
        }

        pthread_join(w->thread, NULL);
        w->thread_started = false;
        w->running = false;
    } else {
        w->running = false;
    }

    rec_write_item_t item;
    while (rec_write_queue_dequeue(&w->queue, &item)) {
        free(item.data);
    }

    if (w->eventfd >= 0)
        close(w->eventfd);
    rec_segment_destroy(&w->segment);
    free(w);
    *writer = NULL;
}

int rec_writer_get_eventfd(const rec_writer_t *writer)
{
    return writer ? writer->eventfd : -1;
}

int rec_writer_publish_codec_config(rec_writer_t *writer,
                                    uint32_t      format,
                                    const uint8_t *data,
                                    uint32_t      size)
{
    if (!writer)
        return -EINVAL;
    return rec_codec_config_publish(&writer->codec_config, format, data, size);
}

int rec_writer_enqueue_live(rec_writer_t *writer,
                            const uint8_t *data,
                            uint32_t       size,
                            uint64_t       timestamp_ns,
                            bool           is_keyframe,
                            bool           is_segment_boundary)
{
    if (!writer || !data || size == 0)
        return -EINVAL;

    if (writer->overflow_drop) {
        if (!is_keyframe) {
            atomic_fetch_add_explicit(&writer->dropped_frames, 1u, memory_order_relaxed);
            return REC_ERR_QUEUE_FULL;
        }
        is_segment_boundary = true;
    } else if (rec_write_queue_full(&writer->queue)) {
        writer->overflow_drop = true;
        atomic_fetch_add_explicit(&writer->dropped_frames, 1u, memory_order_relaxed);
        return REC_ERR_QUEUE_FULL;
    }

    rec_write_item_t item;
    memset(&item, 0, sizeof(item));
    item.data = malloc(size);
    if (!item.data)
        return -ENOMEM;

    memcpy(item.data, data, size);
    item.size = size;
    item.timestamp_ns = timestamp_ns;
    item.is_keyframe = is_keyframe;
    item.is_segment_boundary = is_segment_boundary;

    if (!rec_write_queue_try_enqueue(&writer->queue, &item)) {
        free(item.data);
        writer->overflow_drop = true;
        atomic_fetch_add_explicit(&writer->dropped_frames, 1u, memory_order_relaxed);
        return REC_ERR_QUEUE_FULL;
    }

    if (writer->overflow_drop && is_keyframe)
        writer->overflow_drop = false;

    return rec_writer_signal(writer);
}

uint64_t rec_writer_get_written_bytes(const rec_writer_t *writer)
{
    return writer
        ? atomic_load_explicit(&writer->written_bytes, memory_order_acquire)
        : 0;
}

uint32_t rec_writer_get_dropped_frames(const rec_writer_t *writer)
{
    return writer
        ? atomic_load_explicit(&writer->dropped_frames, memory_order_acquire)
        : 0;
}

bool rec_writer_is_overflow_dropping(const rec_writer_t *writer)
{
    return writer ? writer->overflow_drop : false;
}
