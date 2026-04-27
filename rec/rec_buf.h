#ifndef REC_BUF_H
#define REC_BUF_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "rec_defs.h"

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint64_t timestamp_ns;
    uint64_t seq_num;
    uint32_t batch_gen;     /* 0 = not pre-roll; >0 = pre-roll batch id */
    bool     is_keyframe;
} rec_frame_entry_t;

typedef struct {
    rec_frame_entry_t  entries[REC_PRE_QUEUE_DEPTH];
    _Atomic uint32_t   head;   /* Writer reads here */
    _Atomic uint32_t   tail;   /* event loop writes here */
} rec_pre_queue_t;

typedef struct rec_buf {
    uint8_t           *ring;
    uint32_t           ring_size;
    uint32_t           write_pos;

    rec_frame_entry_t  index[REC_FRAME_INDEX_MAX];
    uint32_t           index_head;
    uint32_t           index_tail;
    uint32_t           index_count;

    _Atomic uint32_t   protected_read_offset;
    _Atomic uint32_t   protected_read_size;
    _Atomic uint32_t   protected_gen;

    _Atomic uint32_t   next_pre_gen;
    _Atomic uint32_t   aborted_pre_gen;

    rec_pre_queue_t    pre_queue;
} rec_buf_t;

/* API */
rec_buf_t *rec_buf_create(uint32_t ring_size);
void       rec_buf_destroy(rec_buf_t **buf);

/*
 * rec_buf_push(): Push one encoded frame into the ring.
 * - data/size: frame bytes
 * - timestamp_ns, seq_num: frame metadata
 * - is_keyframe: true if IDR/I-frame
 * - Returns REC_OK, REC_ERR_WRITER_STUCK (protect timeout)
 * Side effects: evicts oldest index entries if ring wraps around them.
 * Also calls rec_state_force_idle() callback on timeout (pass NULL to skip).
 */
int rec_buf_push(rec_buf_t *buf,
                 const uint8_t *data, uint32_t size,
                 uint64_t timestamp_ns, uint64_t seq_num,
                 bool is_keyframe);

/*
 * rec_buf_extract_from_keyframe():
 * Scans index from oldest entry, finds the latest keyframe whose
 * timestamp_ns <= (now_ns - pre_sec * 1e9), or falls back to oldest keyframe.
 * Returns number of entries enqueued to pre_queue (0 = no keyframe found,
 * caller should go to WAIT_KEYFRAME).
 * Sets protected window and increments next_pre_gen.
 * batch_gen is returned via *batch_gen_out.
 */
int rec_buf_extract_from_keyframe(rec_buf_t *buf,
                                  uint64_t   now_ns,
                                  uint32_t   pre_sec,
                                  uint32_t  *batch_gen_out);

/*
 * rec_buf_abort_pre(): Abort the given pre-roll batch.
 * Sets aborted_pre_gen = batch_gen, clears protect window.
 */
void rec_buf_abort_pre(rec_buf_t *buf, uint32_t batch_gen);

/*
 * rec_pre_queue_dequeue(): Dequeue one entry. Returns true on success.
 */
bool rec_pre_queue_dequeue(rec_pre_queue_t *q, rec_frame_entry_t *out);

/*
 * rec_buf_read_ring(): Copy bytes from the ring buffer.
 * Used by Writer thread to read pre-roll data.
 * Handles wrap-around correctly.
 */
void rec_buf_read_ring(const rec_buf_t *buf, uint32_t offset, uint32_t size, uint8_t *dst);

#endif /* REC_BUF_H */
