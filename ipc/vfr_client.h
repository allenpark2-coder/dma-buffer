/* ipc/vfr_client.h — VFR IPC Client 內部介面
 *
 * 此標頭為 vfr_ctx.c 的內部 include，消費者程式只需 #include "vfr.h"。
 *
 * vfr_client_ref_t 儲存在 vfr_frame_t.priv 中，供 vfr_put_frame() 識別
 * client mode 並發送 vfr_release_msg_t 給 server。
 */
#ifndef VFR_CLIENT_H
#define VFR_CLIENT_H

#include "vfr_defs.h"
#include "vfr.h"
#include "ipc/vfr_ipc_types.h"

/* frame->priv 模式標記（對齊 vfr_pool.h 的 VFR_PRIV_POOL）*/
#define VFR_PRIV_CLIENT  1u

/* ─── client slot back-reference（存入 frame->priv）────────────────────── */
/* 供 vfr_put_frame() close dma_fd 並發送 vfr_release_msg_t */
typedef struct {
    uint32_t mode;       /* VFR_PRIV_CLIENT */
    int      socket_fd;  /* 借用自 vfr_ctx_t，不在此處 close */
    uint32_t session_id;
    uint32_t slot_id;
    uint64_t seq_num;
} vfr_client_ref_t;

/* ─── Client 連線狀態（存入 vfr_ctx_t）────────────────────────────────── */
typedef struct {
    int              socket_fd;
    uint32_t         session_id;
    vfr_shm_header_t shm_hdr;    /* 從 server 收到的格式協商資料 */
    vfr_client_ref_t slot_ref;   /* 當前持有幀的 back-ref（最多一幀） */
} vfr_client_state_t;

/*
 * vfr_client_connect()：
 *   連線到 \0/vfr/<stream_name>，執行握手。
 *   成功後填入 out_state。
 *
 * 回傳：0 = 成功；-1 = 失敗
 */
int vfr_client_connect(const char *stream_name, vfr_client_state_t *out_state);

/*
 * vfr_client_recv_frame()：
 *   從 socket 接收 vfr_frame_msg_t（payload）+ dma_fd（SCM_RIGHTS）。
 *   填入 out_frame；設定 frame->priv = &state->slot_ref。
 *
 *   flags：VFR_FLAG_NONBLOCK 時使用 MSG_DONTWAIT。
 *
 * 回傳：0 = 成功；1 = 無資料（EAGAIN，NONBLOCK 模式）；-1 = 連線錯誤
 */
int vfr_client_recv_frame(vfr_client_state_t *state, vfr_frame_t *out_frame, int flags);

/*
 * vfr_client_send_release()：
 *   向 server 發送 vfr_release_msg_t。
 *   由 vfr_put_frame() 在 frame->priv->mode == VFR_PRIV_CLIENT 時呼叫。
 *
 * 回傳：0 = 成功；-1 = 失敗（EPIPE 表示 server 已離線）
 */
int vfr_client_send_release(vfr_client_ref_t *ref);

/*
 * vfr_client_disconnect()：
 *   關閉 socket，清理狀態。
 */
void vfr_client_disconnect(vfr_client_state_t *state);

#endif /* VFR_CLIENT_H */
