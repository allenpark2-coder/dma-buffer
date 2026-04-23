/* core/vfr_pool.h — Buffer Pool Manager 內部介面
 * vfr_pool_t 為 opaque struct，外部模組不直接存取欄位。
 */
#ifndef VFR_POOL_H
#define VFR_POOL_H

#include "vfr_defs.h"
#include "vfr.h"
#include "platform/platform_adapter.h"

/* ─── frame->priv 模式標記（判斷 pool ref vs client ref）─────────────── */
/* 兩個 ref struct 均以此 uint32_t 為第一個欄位，供 vfr_put_frame() 辨識 */
#define VFR_PRIV_POOL    0u   /* vfr_slot_ref_t（Phase 1 + server side） */
#define VFR_PRIV_CLIENT  1u   /* vfr_client_ref_t（Phase 2 consumer side） */

/* ─── Pool 內部 back-reference（存入 frame->priv）─────────────────────── */
/* 前向宣告 struct vfr_pool，因為 vfr_slot_ref_t 在 pool 內部定義之前就需要被引用 */
typedef struct vfr_pool vfr_pool_t;

typedef struct {
    uint32_t         mode;      /* VFR_PRIV_POOL */
    vfr_pool_t      *pool;
    uint32_t         slot_idx;
} vfr_slot_ref_t;

/* ─── Pool 生命週期 ──────────────────────────────────────────────────────── */

/*
 * vfr_pool_create()：
 *   ops        — 平台 adapter ops（借用指標，pool destroy 前不得釋放）
 *   slot_count — 0 使用 VFR_DEFAULT_SLOTS；上限 VFR_MAX_SLOTS
 *   cfg        — 格式協商（Phase 1 可傳 NULL）
 *
 * 回傳：成功回傳 pool 指標；失敗回傳 NULL
 */
vfr_pool_t *vfr_pool_create(const vfr_platform_ops_t *ops, uint32_t slot_count,
                             const vfr_shm_header_t *cfg);

/*
 * vfr_pool_destroy()：
 *   釋放所有資源。*pool 設為 NULL；重複呼叫為 no-op。
 */
void vfr_pool_destroy(vfr_pool_t **pool);

/* ─── 生產者端操作 ───────────────────────────────────────────────────────── */

/*
 * vfr_pool_acquire()：
 *   找一個 FREE slot 轉為 FILLING，並呼叫 platform->get_frame() 填入 metadata。
 *   out_idx — 輸出 slot index（供後續 dispatch 使用）
 *
 * 回傳：0 = 成功；1 = 暫無新幀（EAGAIN）；-1 = 不可恢復錯誤
 */
int vfr_pool_acquire(vfr_pool_t *pool, uint32_t *out_idx);

/*
 * vfr_pool_dispatch_single()：
 *   單 process / 單 consumer 版本：將 slot 從 FILLING/READY → IN_FLIGHT（refcount = 1），
 *   並將 frame_meta 複製到 out_frame。
 *
 * 回傳：0 = 成功；-1 = 錯誤
 */
int vfr_pool_dispatch_single(vfr_pool_t *pool, uint32_t slot_idx, vfr_frame_t *out_frame);

/* ─── 消費者端操作 ───────────────────────────────────────────────────────── */

/*
 * vfr_pool_put_slot()：
 *   由 vfr_put_frame() 內部呼叫（Phase 1 standalone mode）。
 *   原子遞減 refcount；若降為 0，呼叫 platform->put_frame() 並將 slot 轉為 FREE。
 *   frame->dma_fd 在此函式中 close 並設為 -1。
 */
void vfr_pool_put_slot(vfr_pool_t *pool, vfr_frame_t *frame);

/* ─── Phase 2 Server 端操作 ──────────────────────────────────────────────── */

/*
 * vfr_pool_cancel_acquire()：
 *   在 vfr_pool_acquire() 成功後、但無 consumer 需要接收時呼叫。
 *   直接呼叫 platform->put_frame() 並將 slot 設回 FREE。
 *   不得與 vfr_pool_begin_dispatch() 混用。
 */
void vfr_pool_cancel_acquire(vfr_pool_t *pool, uint32_t slot_idx);

/*
 * vfr_pool_begin_dispatch()：
 *   Server 在 sendmsg 之前呼叫。
 *   設定 slot->refcount = n_consumers，並將 slot 轉為 IN_FLIGHT。
 *   必須在第一個 sendmsg 之前完成（POOL_DESIGN.md §3.2 Phase B）。
 *
 * 回傳：0 = 成功；-1 = slot 狀態不符（不在 READY state）
 */
int vfr_pool_begin_dispatch(vfr_pool_t *pool, uint32_t slot_idx, uint32_t n_consumers);

/*
 * vfr_pool_slot_meta()：
 *   取得 slot 的 frame metadata（唯讀）。
 *   呼叫者不得修改或釋放回傳指標。
 *
 * 回傳：metadata 指標；slot_idx 越界時回傳 NULL
 */
const vfr_frame_t *vfr_pool_slot_meta(vfr_pool_t *pool, uint32_t slot_idx);

/*
 * vfr_pool_slot_dma_fd()：
 *   取得 slot 的 producer 端 dma_fd（用於 SCM_RIGHTS sendmsg）。
 *   此 fd 由 pool 持有，呼叫者不得 close。
 *
 * 回傳：fd（>= 0）；越界或無效時回傳 -1
 */
int vfr_pool_slot_dma_fd(vfr_pool_t *pool, uint32_t slot_idx);

/*
 * vfr_pool_server_release()：
 *   Server 收到 vfr_release_msg_t 時呼叫（Phase 2）。
 *   原子遞減 slot refcount；若降為 0，呼叫 platform->put_frame() + SLOT_FREE。
 *   seq_num = 0 時跳過 seq 驗證（force release，斷線清理用）。
 *
 *   注意：此函式不 close consumer 端的 dma_fd（那是 consumer 的責任）。
 */
void vfr_pool_server_release(vfr_pool_t *pool, uint32_t slot_id, uint64_t seq_num);

/*
 * vfr_pool_force_release()：
 *   Phase 3 DROP_OLDEST 使用。
 *   先設定 slot->tombstone = true，再原子遞減 refcount。
 *   若降為 0，通知 platform put_frame 並轉為 FREE，同時清除 tombstone。
 *
 *   效果：
 *   - 此後若有遲到的 vfr_release_msg_t（consumer 舊幀的 put_frame），
 *     server_release() 看到 tombstone=true 會直接 skip，不做雙重釋放。
 *   - 若 tombstone 設好後 refcount 已為 0（其他 consumer 更早釋放），
 *     此函式負責最終清理。
 */
void vfr_pool_force_release(vfr_pool_t *pool, uint32_t slot_id);

#endif /* VFR_POOL_H */
