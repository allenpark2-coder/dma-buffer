/* ipc/vfr_ipc_types.h — IPC 協議型別定義（Phase 2）
 *
 * 所有跨 process 傳輸的訊息結構集中於此。
 * Server 和 client 均 include 此標頭，禁止在其他地方重複定義。
 *
 * 握手流程（見 POOL_DESIGN.md §五）：
 *   client → server : vfr_client_hello_t
 *   server → client : vfr_handshake_t（成功）或 vfr_error_response_t（失敗）
 *   server → client : vfr_shm_header_t（格式協商）
 *
 * 幀傳遞流程：
 *   server → client : vfr_frame_msg_t（payload）+ dma_fd（SCM_RIGHTS ancillary）
 *   client → server : vfr_release_msg_t（vfr_put_frame 後回報）
 */
#ifndef VFR_IPC_TYPES_H
#define VFR_IPC_TYPES_H

#include "vfr_defs.h"

/* ─── Client → Server：連線後第一個訊息 ─────────────────────────────────── */
typedef struct {
    uint32_t magic;           /* VFR_SHM_MAGIC，快速過濾非 VFR 連線 */
    uint16_t proto_version;   /* VFR_PROTO_VERSION */
    uint16_t reserved;        /* 填 0，保留對齊 */
    int32_t  consumer_pid;    /* 供 server 做 pidfd_open()（Phase 4）*/
    uint32_t consumer_uid;    /* 供 SO_PEERCRED 補充驗證（選做）*/
    uint32_t policy;          /* Phase 3 新增：vfr_backpressure_t（drop_oldest=0 預設）*/
    uint32_t _pad2;           /* 對齊至 8-byte 邊界 */
} vfr_client_hello_t;

/* ─── Server → Client：握手成功回應 ─────────────────────────────────────── */
typedef struct {
    uint32_t magic;           /* VFR_SHM_MAGIC */
    uint16_t proto_version;   /* VFR_PROTO_VERSION */
    uint16_t header_size;     /* sizeof(vfr_shm_header_t)；client 據此判斷相容性 */
    uint32_t session_id;      /* server 分配的唯一識別碼，供 vfr_release_msg_t 驗證 */
    uint32_t _pad;            /* 對齊 */
} vfr_handshake_t;

/* ─── Server → Client：Phase 3 新增 — eventfd 設定（握手最後一步）─────────
 * server 在發完 vfr_shm_header_t 後，發送此訊息並在 SCM_RIGHTS 中攜帶 eventfd fd。
 * consumer 收到後將 eventfd 存入 vfr_client_state_t.eventfd。
 * 此 eventfd 為 producer→consumer 新幀通知；每幀 dispatch 後 server 寫入一次。
 */
typedef struct {
    uint32_t magic;   /* VFR_SHM_MAGIC */
    uint32_t _pad;
} vfr_eventfd_setup_t;

/* ─── Server → Client：版本不符時發送後關閉連線 ─────────────────────────── */
typedef struct {
    uint32_t magic;   /* VFR_SHM_MAGIC */
    uint32_t error;   /* 1 = proto_version mismatch；未來可擴充 */
} vfr_error_response_t;

/* ─── Server → Client：幀傳遞訊息（配合 SCM_RIGHTS 傳遞 dma_fd）────────── */
/* dma_fd 透過 SCM_RIGHTS ancillary data 傳遞，不在本結構中 */
typedef struct {
    uint32_t magic;                         /* VFR_SHM_MAGIC，防止雜訊誤解析 */
    uint32_t slot_id;                       /* pool slot index，供 vfr_release_msg_t 使用 */
    uint32_t session_id;                    /* 與 vfr_handshake_t 一致 */
    uint32_t _pad;
    /* 以下對應 vfr_frame_t 欄位（dma_fd 由 SCM_RIGHTS 傳遞） */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint32_t buf_size;
    uint32_t flags;
    uint32_t plane_offset[VFR_MAX_PLANES];
    uint64_t timestamp_ns;
    uint64_t seq_num;
} vfr_frame_msg_t;

/* ─── Client → Server：vfr_put_frame() 後回報（見 POOL_DESIGN.md §六）──── */
typedef struct {
    uint32_t magic;       /* VFR_SHM_MAGIC，防止雜訊誤解析 */
    uint32_t session_id;  /* consumer_session.session_id，server 驗證來源合法性 */
    uint32_t slot_id;     /* pool 中的 slot 索引（0 ~ slot_count-1）*/
    uint32_t reserved;    /* 填 0，保持 8-byte 對齊 */
    uint64_t seq_num;     /* frame seq_num，防止過期回收（見 POOL_DESIGN.md §六）*/
} vfr_release_msg_t;

/* ─── AI 觸發訊息（Recorder Event Trigger，§5.2）────────────────────────── */
#define VFR_EVENT_MAGIC  0x52454354u   /* "RECT" */

typedef struct {
    uint32_t magic;                         /* VFR_EVENT_MAGIC */
    uint32_t event_type;                    /* rec_trigger_type_t */
    uint64_t timestamp_ns;                  /* CLOCK_MONOTONIC ns */
    float    confidence;                    /* 0.0 ~ 1.0 */
    char     stream_name[VFR_SOCKET_NAME_MAX];
    char     label[32];
} vfr_event_msg_t;

#endif /* VFR_IPC_TYPES_H */
