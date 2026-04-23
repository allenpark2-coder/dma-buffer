/* core/vfr_pool.c — Buffer Pool Manager（Slot Allocator）
 *
 * 依 POOL_DESIGN.md §一 的狀態機與 refcount 設計實作。
 *
 * Phase 1 範圍：單 process，單 consumer，無 IPC、無 eventfd。
 *
 * fd 生命週期設計（對齊 Phase 2 SCM_RIGHTS 語意）：
 *   slot->frame_meta.dma_fd  — producer 端永久持有；pool destroy 或 platform->put_frame() 時 close
 *   out_frame->dma_fd        — consumer 持有的 dup'd fd；vfr_put_frame() 時 close
 *   dispatch_single() 中用 dup() 模擬 SCM_RIGHTS 的 kernel-level fd 複製
 */

#include "vfr_pool.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdatomic.h>

/* ─── Slot 狀態機（見 POOL_DESIGN.md §1.1）─────────────────────────────── */
typedef enum {
    SLOT_FREE        = 0,
    SLOT_FILLING     = 1,
    SLOT_READY       = 2,
    SLOT_IN_FLIGHT   = 3,
} slot_state_t;

/* ─── 內部 back-reference（嵌入 islot，供 frame->priv 指向）──────────────── */
typedef struct {
    struct vfr_pool *pool;
    uint32_t         slot_idx;
} vfr_slot_ref_t;

/* ─── Internal slot（含 back-ref）──────────────────────────────────────── */
typedef struct {
    _Atomic int         state;       /* slot_state_t */
    _Atomic uint32_t    refcount;
    _Atomic bool        tombstone;
    vfr_frame_t         frame_meta;  /* producer 端 metadata + fd */
    vfr_slot_ref_t      ref;         /* frame->priv 指向此處 */
} vfr_islot_t;

/* ─── Pool 完整定義（opaque，外部只見 vfr_pool_t*）──────────────────────── */
struct vfr_pool {
    uint32_t                 slot_count;
    const vfr_platform_ops_t *platform;
    void                     *platform_ctx;
    vfr_islot_t              islots[];  /* flexible array */
};

/* ─── helper：CAS 搶 FREE slot → FILLING ───────────────────────────────── */
static int find_free_slot(struct vfr_pool *p)
{
    for (uint32_t i = 0; i < p->slot_count; i++) {
        int expected = SLOT_FREE;
        if (atomic_compare_exchange_strong_explicit(
                &p->islots[i].state, &expected, SLOT_FILLING,
                memory_order_acq_rel, memory_order_relaxed)) {
            return (int)i;
        }
    }
    return -1;
}

/* ─── vfr_pool_create ────────────────────────────────────────────────────── */
vfr_pool_t *vfr_pool_create(const vfr_platform_ops_t *ops, uint32_t slot_count,
                             const vfr_shm_header_t *cfg)
{
    if (!ops) {
        VFR_LOGE("ops is NULL");
        return NULL;
    }

    if (slot_count == 0) slot_count = VFR_DEFAULT_SLOTS;
    if (slot_count > VFR_MAX_SLOTS) {
        VFR_LOGE("slot_count %u exceeds VFR_MAX_SLOTS %u", slot_count, VFR_MAX_SLOTS);
        return NULL;
    }

    size_t total = sizeof(struct vfr_pool) + (size_t)slot_count * sizeof(vfr_islot_t);
    struct vfr_pool *p = calloc(1, total);
    if (!p) {
        VFR_LOGE("calloc failed: %s", strerror(errno));
        return NULL;
    }

    p->slot_count = slot_count;
    p->platform   = ops;

    /* 初始化每個 slot */
    for (uint32_t i = 0; i < slot_count; i++) {
        atomic_store(&p->islots[i].state,     (int)SLOT_FREE);
        atomic_store(&p->islots[i].refcount,  0u);
        atomic_store(&p->islots[i].tombstone, false);
        p->islots[i].frame_meta.dma_fd = -1;
        p->islots[i].ref.pool          = p;
        p->islots[i].ref.slot_idx      = i;
    }

    /* 初始化 platform adapter */
    if (ops->init(&p->platform_ctx, cfg) != 0) {
        VFR_LOGE("platform '%s' init failed", ops->name);
        free(p);
        return NULL;
    }

    VFR_LOGI("pool created: platform=%s slots=%u", ops->name, slot_count);
    return p;
}

/* ─── vfr_pool_destroy ───────────────────────────────────────────────────── */
void vfr_pool_destroy(vfr_pool_t **pool)
{
    if (!pool || !*pool) return;
    struct vfr_pool *p = *pool;

    /* 原則五釋放順序：先通知 platform，再 close fd，再 free */
    if (p->platform && p->platform->destroy) {
        p->platform->destroy(&p->platform_ctx);
    }

    /* 回收所有仍持有 dma_fd 的 slot（platform destroy 後 fd 可能已失效，仍需 close） */
    for (uint32_t i = 0; i < p->slot_count; i++) {
        int fd = p->islots[i].frame_meta.dma_fd;
        if (fd >= 0) {
            close(fd);
            p->islots[i].frame_meta.dma_fd = -1;
        }
    }

    free(p);
    *pool = NULL;
    VFR_LOGI("pool destroyed");
}

/* ─── vfr_pool_acquire ───────────────────────────────────────────────────── */
int vfr_pool_acquire(vfr_pool_t *pool, uint32_t *out_idx)
{
    struct vfr_pool *p = pool;

    int idx = find_free_slot(p);
    if (idx < 0) {
        VFR_LOGW("no free slot (slot_count=%u); caller should retry or drop", p->slot_count);
        return 1;  /* EAGAIN */
    }

    /* 呼叫 platform->get_frame() 填入 frame metadata + producer fd */
    vfr_frame_t *meta = &p->islots[idx].frame_meta;
    int ret = p->platform->get_frame(p->platform_ctx, meta);
    if (ret != 0) {
        /* 取幀失敗：歸還 slot */
        atomic_store_explicit(&p->islots[idx].state, (int)SLOT_FREE, memory_order_release);
        return ret;
    }

    /* FILLING → READY */
    atomic_store_explicit(&p->islots[idx].state, (int)SLOT_READY, memory_order_release);

    VFR_LOGD("acquire slot[%d] seq=%llu dma_fd=%d",
             idx, (unsigned long long)meta->seq_num, meta->dma_fd);
    *out_idx = (uint32_t)idx;
    return 0;
}

/* ─── vfr_pool_dispatch_single ──────────────────────────────────────────── */
int vfr_pool_dispatch_single(vfr_pool_t *pool, uint32_t slot_idx, vfr_frame_t *out_frame)
{
    struct vfr_pool *p = pool;

    if (slot_idx >= p->slot_count) {
        VFR_LOGE("slot_idx %u out of range", slot_idx);
        return -1;
    }

    vfr_islot_t *slot = &p->islots[slot_idx];

    /* dup() fd：模擬 Phase 2 SCM_RIGHTS 的 kernel-level fd 複製
     * producer 保留 slot->frame_meta.dma_fd；consumer 持有 dup'd fd
     * 兩者指向同一個底層 file（memfd / dma_buf），close 互不影響 */
    int consumer_fd = -1;
    if (slot->frame_meta.dma_fd >= 0) {
        consumer_fd = dup(slot->frame_meta.dma_fd);
        if (consumer_fd < 0) {
            VFR_LOGE("dup(dma_fd=%d) failed: %s", slot->frame_meta.dma_fd, strerror(errno));
            atomic_store_explicit(&slot->state, (int)SLOT_FREE, memory_order_release);
            return -1;
        }
    }

    /* Phase B（POOL_DESIGN.md §3.2）：先設 refcount，再標記 IN_FLIGHT */
    atomic_store_explicit(&slot->refcount, 1u, memory_order_release);
    atomic_store_explicit(&slot->state, (int)SLOT_IN_FLIGHT, memory_order_release);

    /* 複製 frame metadata 給 consumer，替換為 consumer 持有的 dup'd fd */
    *out_frame       = slot->frame_meta;
    out_frame->dma_fd = consumer_fd;
    out_frame->priv   = &slot->ref;   /* back-ref for vfr_put_frame() */

    VFR_LOGD("dispatch slot[%u] seq=%llu producer_fd=%d consumer_fd=%d",
             slot_idx, (unsigned long long)out_frame->seq_num,
             slot->frame_meta.dma_fd, consumer_fd);
    return 0;
}

/* ─── vfr_pool_put_slot ─────────────────────────────────────────────────── */
void vfr_pool_put_slot(vfr_pool_t *pool_hint, vfr_frame_t *frame)
{
    (void)pool_hint;  /* pool 從 frame->priv 的 back-ref 取得 */

    if (!frame || frame->dma_fd < 0) return;

    /* 取回 slot back-reference */
    vfr_slot_ref_t *ref = (vfr_slot_ref_t *)frame->priv;
    if (!ref) {
        /* 無 priv（不經由 pool 分配的 frame）：只 close fd */
        VFR_LOGW("put_slot: no back-ref, closing fd=%d directly", frame->dma_fd);
        close(frame->dma_fd);
        frame->dma_fd = -1;
        return;
    }

    struct vfr_pool *p = ref->pool;
    uint32_t idx = ref->slot_idx;
    vfr_islot_t *slot = &p->islots[idx];

    /* 先 close consumer 的 dup'd fd */
    close(frame->dma_fd);
    frame->dma_fd = -1;
    frame->priv   = NULL;

    /* tombstone check（DROP_OLDEST force_release 後，遲到的 put_frame）*/
    if (atomic_load_explicit(&slot->tombstone, memory_order_acquire)) {
        VFR_LOGD("put_slot slot[%u]: tombstone set, skip refcount", idx);
        return;
    }

    /* 原子遞減 refcount；若降為 0 → 歸還 slot */
    uint32_t prev = atomic_fetch_sub_explicit(&slot->refcount, 1u, memory_order_acq_rel);
    if (prev == 1) {
        /* 最後一個 consumer：通知 platform 可回收 producer 端 buffer */
        if (p->platform && p->platform->put_frame) {
            p->platform->put_frame(p->platform_ctx, &slot->frame_meta);
            /* mock / real put_frame 負責 close slot->frame_meta.dma_fd */
        }
        slot->frame_meta.dma_fd = -1;
        atomic_store_explicit(&slot->tombstone, false, memory_order_release);
        atomic_store_explicit(&slot->state, (int)SLOT_FREE, memory_order_release);
        VFR_LOGD("put_slot slot[%u]: freed", idx);
    }
}
