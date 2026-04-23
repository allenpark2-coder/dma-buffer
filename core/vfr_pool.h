/* core/vfr_pool.h — Buffer Pool Manager 內部介面
 * vfr_pool_t 為 opaque struct，外部模組不直接存取欄位。
 */
#ifndef VFR_POOL_H
#define VFR_POOL_H

#include "vfr_defs.h"
#include "vfr.h"
#include "platform/platform_adapter.h"

/* opaque pool handle；實際定義在 vfr_pool.c */
typedef struct vfr_pool vfr_pool_t;

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
 *   由 vfr_put_frame() 內部呼叫。
 *   原子遞減 refcount；若降為 0，呼叫 platform->put_frame() 並將 slot 轉為 FREE。
 *   frame->dma_fd 在此函式中 close 並設為 -1。
 */
void vfr_pool_put_slot(vfr_pool_t *pool, vfr_frame_t *frame);

#endif /* VFR_POOL_H */
