# VFR — Buffer Pool、Dispatch、FD 生命週期設計細節

> 本文件補充 `plan-vfr-cross-platform-v2.md` 未明確的實作核心：  
> Slot 狀態機、FD 生命週期（SCM_RIGHTS）、Dispatch 機制、IPC 握手協議、  
> 跨 Process 回收協議、並發模型、Amba Adapter 邊界。  
> **AI 實作前必須閱讀此文件，不得自行推斷這些細節。**

---

## 一、Slot 狀態機

### 1.1 狀態定義

```c
typedef enum {
    SLOT_FREE        = 0,  /* 未被任何人持有，pool 可分配 */
    SLOT_FILLING     = 1,  /* producer 正在填入（DSP DMA 進行中） */
    SLOT_READY       = 2,  /* producer 填完，等待 dispatch 給 consumer */
    SLOT_IN_FLIGHT   = 3,  /* 已 dispatch，至少一個 consumer 持有中 */
} slot_state_t;
```

### 1.2 狀態轉移圖

```
            producer_acquire()
  FREE ─────────────────────────► FILLING
   ▲                                  │
   │                                  │ producer 填完，呼叫 dispatch()
   │                                  ▼
   │                              READY
   │                                  │
   │                                  │ dispatch 完成：refcount 設為 N（N = consumer 數）
   │                                  │ 若 N == 0（無 consumer），直接回 FREE
   │                                  ▼
   │    refcount 原子減到 0      IN_FLIGHT  ◄── 每個 consumer 持有期間
   └────────────────────────────────┘
         最後一個 consumer 呼叫 vfr_put_frame()
```

### 1.3 Slot 資料結構

```c
typedef struct {
    slot_state_t        state;       /* 當前狀態 */
    _Atomic uint32_t    refcount;    /* 持有此 slot 的 consumer 數量 */
    bool                tombstone;   /* true = 已被 force_release 標記；
                                      * 遲到的 vfr_put_frame() 看到此旗標須跳過 refcount 操作 */
    int                 dma_fd;      /* producer 側永久持有的 fd（不隨 dispatch 關閉） */
    uint32_t            buf_size;    /* mmap 長度，由 platform adapter 填入 */
    uint64_t            seq_num;     /* 幀序號，dispatch 時從 SHM header seq 取得 */
    vfr_frame_t         frame_meta;  /* 完整 frame metadata，dispatch 時複製給 consumer */
} vfr_slot_t;
```

### 1.4 refcount 操作規則

```
dispatch() 時：
    atomic_store(&slot->refcount, N)   // N = 當前 active consumer 數
    slot->state = SLOT_IN_FLIGHT

vfr_put_frame() 時（consumer 端）：
    prev = atomic_fetch_sub(&slot->refcount, 1)
    if (prev == 1) {
        // 我是最後一個，通知 pool 回收
        slot->state = SLOT_FREE
        // （若需要）通知 BLOCK_PRODUCER：write(pool->free_eventfd, 1)
    }
```

**禁止**用 mutex 保護 refcount，必須用 `_Atomic`；狀態轉移在正確的使用情境下是單向的，不需要額外鎖。

---

## 二、SCM_RIGHTS FD 生命週期

### 2.1 三方 fd 關係

```
┌─────────────────────────────────────────────────────────┐
│  DSP Buffer（kernel dma_buf object，refcount 由 kernel 管）│
└────────────────────────┬────────────────────────────────┘
                         │
         ┌───────────────┼───────────────────┐
         │               │                   │
    producer_fd      consumer_fd_1       consumer_fd_2
  （pool 永久持有）   （sendmsg 產生）    （sendmsg 產生）
  pool 存活期間       consumer put_frame  consumer put_frame
  不 close            後 close            後 close
```

### 2.2 正確操作步驟

**Producer / Server 端（dispatch 一個 slot 給一個 consumer）**：

```c
// 1. 透過 SCM_RIGHTS 傳送 fd
//    kernel 在 consumer 的 fd table 中建立新 fd，指向同一個 dma_buf
//    producer 的 slot->dma_fd 不變，不需要 close 或 dup
sendmsg(consumer_socket_fd, &msg_with_scm_rights, 0);

// 2. producer 保留 slot->dma_fd，下一幀還會用到
// 3. 不需要 dup()，SCM_RIGHTS 傳送本身就是 kernel 層的 dup
```

**Consumer 端（收到 fd）**：

```c
// 1. recvmsg 取得的 fd 是 consumer fd table 中的新 fd（編號可能不同）
int local_fd = recvmsg_extract_fd(...);

// 2. 直接使用，不需要再 dup()
frame->dma_fd = local_fd;

// 3. vfr_put_frame() 時，框架負責 close
close(frame->dma_fd);
frame->dma_fd = -1;
```

### 2.3 Kernel refcount 說明

- `dma_buf` 的 kernel refcount 在每次 `sendmsg(SCM_RIGHTS)` 後 +1（consumer 收到 fd 時）
- 每次 `close(consumer_fd)` 後 -1
- Producer `close(slot->dma_fd)` 時 -1（只在 pool destroy 時發生）
- refcount 歸零時 kernel 釋放 buffer

**結論**：傳給 3 個 consumer，kernel refcount = producer_fd(1) + consumer_fd_1(1) + consumer_fd_2(1) + consumer_fd_3(1) = 4。每個 consumer put_frame 後各 -1，pool destroy 時 -1，最終歸零。

---

## 三、Dispatch 機制

### 3.1 EventFD 設計：每個 Consumer 各有一個

```
stream 的 consumer list：

consumer_session[0]:  socket_fd, eventfd_0, policy, refslot[]
consumer_session[1]:  socket_fd, eventfd_1, policy, refslot[]
consumer_session[2]:  socket_fd, eventfd_2, policy, refslot[]
```

**不使用一個共用 eventfd 的原因**：  
`eventfd` 在多個 consumer `epoll_wait` 同一個 fd 時，只有一個會被喚醒（level-triggered 雖然都喚醒，但計數消耗語意不適合廣播）。每個 consumer 各自的 eventfd 是最清晰的廣播方案。

### 3.2 Dispatch 偽碼（修正 refcount race）

**關鍵約束**：`refcount` 必須在第一個 `write(eventfd)` 之前設定好，否則 consumer 收到通知後呼叫 `put_frame`，會對 refcount=0 做 `fetch_sub`，結果 wrap 到 UINT32_MAX，slot 永遠不回收。

```
function dispatch(slot):
    // ── Phase A：決策（不傳送，只規劃誰要收到）──────────────────
    N = 0
    receivers = []   // 本次要接收的 consumer 清單

    for each consumer in active_consumer_list:
        if atomic_load(&consumer.tombstone):
            continue   // Phase 4 watchdog 已標記死亡，跳過

        if consumer.policy == DROP_OLDEST:
            if consumer.refslot_count > 0:
                force_release(consumer.refslot[0], consumer)  // 先回收最舊 slot
            receivers.append(consumer)
            N += 1

        elif consumer.policy == SKIP_SELF:
            if consumer.refslot_count > 0:
                continue   // 自己還有 slot 未消化，跳過本幀
            receivers.append(consumer)
            N += 1

        elif consumer.policy == BLOCK_PRODUCER:
            if consumer.refslot_count > 0:
                // epoll_wait(consumer.free_eventfd, timeout=1000/fps_ms)
                ok = wait_with_timeout(consumer.free_eventfd, 1000/fps_ms)
                if not ok:
                    log WARN "session %u BLOCK_PRODUCER timeout, downgrade to DROP_OLDEST"
                    force_release(consumer.refslot[0], consumer)
            receivers.append(consumer)
            N += 1

    // ── Phase B：先設 refcount，再傳送 ──────────────────────────
    // 必須在任何 write(eventfd) 之前完成，否則 consumer 可能提前 put_frame
    if N == 0:
        slot->state = SLOT_FREE
        return

    atomic_store(&slot->refcount, N, memory_order_release)
    slot->state = SLOT_IN_FLIGHT   // _Atomic，用 store(release)

    // ── Phase C：傳送 fd 與通知 ──────────────────────────────────
    for each consumer in receivers:
        consumer.refslot.push(slot)

        ret = send_fd_to_consumer(consumer.socket_fd, slot->dma_fd, slot->frame_meta)
        if ret < 0:  // EPIPE：consumer 在 Phase A 到 Phase C 之間死亡
            consumer.refslot.pop(slot)
            prev = atomic_fetch_sub(&slot->refcount, 1, memory_order_acq_rel)
            if prev == 1:
                slot->state = SLOT_FREE   // 所有 consumer 都死了
            continue

        // fd 送出後才通知，確保 consumer recvmsg 能取到 fd
        write(consumer.eventfd, &(uint64_t){1}, 8)
```

**force_release 偽碼**：

```
function force_release(slot, consumer):
    atomic_store(&slot->tombstone, true, memory_order_release)
    consumer.refslot.remove(slot)
    prev = atomic_fetch_sub(&slot->refcount, 1, memory_order_acq_rel)
    if prev == 1:
        slot->state = SLOT_FREE
        atomic_store(&slot->tombstone, false, memory_order_release)  // 重置供下次使用
```

### 3.3 Dispatch list 資料結構

```c
/* VFR_MAX_CONSUMER_SLOTS / VFR_MAX_CONSUMERS 定義於 include/vfr_defs.h，此處不重複定義 */
#include "vfr_defs.h"

typedef struct {
    int              socket_fd;
    int              eventfd;      /* producer → consumer 新幀通知（每 consumer 獨立） */
    int              free_eventfd; /* consumer → server slot 釋放通知（BLOCK_PRODUCER policy 用）*/
    uint32_t         session_id;   /* server 分配的唯一識別碼，用於 vfr_release_msg_t 驗證 */
    pid_t            pid;          /* 用於 pidfd_open()（Phase 4） */
    int              pidfd;        /* Phase 4：偵測 consumer 存活 */
    vfr_backpressure_t policy;
    vfr_slot_t      *refslot[VFR_MAX_CONSUMER_SLOTS];  /* 此 consumer 持有的 slot；上限見 vfr_defs.h */
    uint32_t         refslot_count;
    _Atomic bool     tombstone;    /* true = 已死亡，等待清理；watchdog thread 寫，dispatch thread 讀 */
} vfr_consumer_session_t;

typedef struct {
    vfr_consumer_session_t sessions[VFR_MAX_CONSUMERS];
    uint32_t               count;
    /* 不用 linked list：consumer 數量有上限，fixed array 存取 O(1) 且 cache-friendly */
} vfr_dispatch_list_t;
```

---

## 四、Amba Adapter 實作邊界

### 4.1 iav5（CV5 / CV52）

```c
#include <iav_ioctl.h>   /* Ambarella SDK header */

// 查詢 canvas buffer descriptor，取得 dma_buf_fd
struct iav_querydesc query = {0};
query.qid = IAV_DESC_CANVAS;
query.arg.canvas.id = canvas_id;   /* 通常為 0（main canvas） */

if (ioctl(iav_fd, IAV_IOC_QUERY_DESC, &query) < 0) {
    // error
}

// SDK >= 某版本後，canvas desc 直接含有 dma_buf_fd
int dma_fd = query.arg.canvas.buf.fd;

// 若 SDK 版本不支援直接 export，需另行 ioctl：
// ioctl(iav_fd, IAV_IOC_EXPORT_DMAFD, &export_arg)
// ← 需確認目標 SDK 版本，查 iav_ioctl.h 是否有 IAV_IOC_EXPORT_DMAFD
```

> ⚠️ **確認清單（實作前必查）**：
> - `iav_ioctl.h` 中 `iav_canvas_desc` 是否有 `.buf.fd` 欄位
> - 若無，搜尋 `IAV_IOC_EXPORT` 確認 export ioctl 名稱
> - `use_dma_buf_fd = 1` 是否需要在 IAV init 時設定（部分版本需要）

### 4.2 iav6（CV72）

```c
#include <iav_ioctl.h>   /* iav6 SDK，API 與 iav5 相似但部分 struct 欄位不同 */

// CV72 使用相同的 IAV_IOC_QUERY_DESC ioctl，但 query id 可能改用：
// IAV_DESC_FRAME 或 IAV_DESC_CANVAS（視 SDK 版本）
struct iav_querydesc query = {0};
query.qid = IAV_DESC_CANVAS;       /* 若編譯失敗，試 IAV_DESC_FRAME */
query.arg.canvas.id = canvas_id;

if (ioctl(iav_fd, IAV_IOC_QUERY_DESC, &query) < 0) { ... }

int dma_fd = query.arg.canvas.buf.fd;
```

> ⚠️ **iav5 vs iav6 差異（實作前必查）**：
> - CV72 SDK 的 `iav_ioctl.h` 路徑可能在 `out/cv72_cnn_tbl/` 下，與 CV5 不同目錄
> - struct `iav_canvas_desc` 在 iav6 中欄位順序可能不同，不可直接複製 iav5 code
> - 建議用 `#ifdef CV72` / `#ifdef CV5` 條件編譯，或各自建立獨立的 `.c` 實作

### 4.3 Platform Adapter get_frame() 偽碼

```c
static int amba_get_frame(void *ctx, vfr_frame_t *out) {
    amba_ctx_t *a = ctx;

    struct iav_querydesc query = {0};
    query.qid = IAV_DESC_CANVAS;
    query.arg.canvas.id = a->canvas_id;

    if (ioctl(a->iav_fd, IAV_IOC_QUERY_DESC, &query) < 0) {
        if (errno == EAGAIN) return 1;   /* 暫無新幀 */
        return -1;                        /* 不可恢復 */
    }

    /* 每幀填入 vfr_frame_t，不從 SHM header 讀靜態值 */
    out->dma_fd         = query.arg.canvas.buf.fd;
    out->width          = query.arg.canvas.width;
    out->height         = query.arg.canvas.height;
    out->stride         = query.arg.canvas.pitch;  /* luma pitch */
    out->buf_size       = query.arg.canvas.buf.length;
    out->format         = V4L2_PIX_FMT_NV12;       /* Amba 預設 NV12 */
    out->plane_offset[0] = 0;
    out->plane_offset[1] = out->stride * out->height;
    out->plane_offset[2] = 0;                       /* NV12 只有 2 plane */
    out->timestamp_ns   = (uint64_t)query.arg.canvas.dsp_pts * 1000u; /* PTS → ns */
    out->flags          = 0;

    return 0;
}
```

> `canvas.buf.length`、`canvas.pitch`、`canvas.dsp_pts` 等欄位名稱需對照實際 SDK header，  
> 不同版本可能叫 `size`、`stride`、`monotonic_pts` 等，**實作前必須 grep SDK header 確認**。

---

## 五、IPC 握手協議（完整雙向）

### 5.1 完整握手流程

```
client                              server
  │                                   │
  │── client_hello ──────────────────►│  (1) client 先自我介紹
  │                                   │  server 驗證 proto_version
  │◄─ vfr_handshake_t ───────────────│  (2) server 回應（或 error_response）
  │◄─ vfr_shm_header_t ─────────────│  (3) 格式協商
  │                                   │
  │  [正常幀傳輸]                      │
  │◄─ SCM_RIGHTS(dma_fd) + frame_meta │
  │◄─ write(eventfd, 1) ─────────────│
  │                                   │
  │── vfr_release_msg_t ────────────►│  (4) consumer put_frame 時回報
```

### 5.2 客戶端 Hello 訊息

```c
/* client → server，連線後第一個訊息 */
typedef struct {
    uint32_t magic;          /* VFR_SHM_MAGIC，快速過濾非 VFR 連線 */
    uint16_t proto_version;  /* VFR_PROTO_VERSION */
    uint16_t reserved;       /* 填 0，保留對齊 */
    pid_t    consumer_pid;   /* 供 server 做 pidfd_open()（Phase 4）*/
    uid_t    consumer_uid;   /* 供 SO_PEERCRED 補充驗證（選做） */
} vfr_client_hello_t;
```

server 收到後：
1. 驗 `magic != VFR_SHM_MAGIC` → 關閉連線，不回應
2. 驗 `proto_version != VFR_PROTO_VERSION` → 發 `vfr_error_response_t`，關閉連線
3. 通過 → 發 `vfr_handshake_t`，再發 `vfr_shm_header_t`，分配 `session_id`

```c
/* server → client，版本不符時發送後關閉連線 */
typedef struct {
    uint32_t magic;   /* VFR_SHM_MAGIC */
    uint32_t error;   /* 1 = proto_version mismatch；未來可擴充 */
} vfr_error_response_t;
```

---

## 六、跨 Process 回收協議（vfr_release_msg_t）

consumer 呼叫 `vfr_put_frame()` 後，框架透過 Unix socket 回傳此訊息給 server：

```c
/* consumer → server，透過 socket 傳送（plain write，非 SCM_RIGHTS） */
typedef struct {
    uint32_t magic;       /* VFR_SHM_MAGIC，防止雜訊誤解析 */
    uint32_t session_id;  /* consumer_session.session_id，server 驗證來源合法性 */
    uint32_t slot_id;     /* pool 中的 slot 索引（0 ~ slot_count-1） */
    uint32_t reserved;    /* 填 0，保持 8-byte 對齊 */
    uint64_t seq_num;     /* frame seq_num，server 驗證不是過期幀的回收 */
} vfr_release_msg_t;
```

**server 收到 vfr_release_msg_t 的處理邏輯**：

```
1. 驗 magic，不符直接丟棄
2. 查 session_id，找不到（tombstone consumer）直接丟棄
3. 驗 slot_id 範圍（0 ~ slot_count-1），越界丟棄 + log WARN
4. 驗 seq_num 與 slot->frame_meta.seq_num 一致，不符丟棄（過期回收）
5. 通過驗證 → 觸發 put_frame 邏輯（refcount fetch_sub）
6. 若 BLOCK_PRODUCER：write(consumer.free_eventfd, 1) 通知等待的 dispatch
```

**為什麼需要 seq_num 驗證**：
consumer crash 後被 watchdog 清理（`force_release`），但 crash 前已送出的 `vfr_release_msg_t` 可能殘留在 socket buffer，被下一次連線的 consumer（相同 session_id slot）誤收，導致誤回收。seq_num 可過濾這類過期訊息。

---

## 七、並發模型與同步語意

### 7.1 前提假設（必須維持，否則以下分析失效）

> **Server 是單執行緒 event loop**（epoll 驅動）。  
> Dispatch、連線管理、release msg 處理全部在同一個 thread。  
> 唯一的例外是 Phase 4 watchdog thread（負責 pidfd 監控與 tombstone 標記）。

### 7.2 各共享欄位的同步手段

| 欄位 | 寫者 | 讀者 | 同步方案 |
|------|------|------|---------|
| `slot->refcount` | dispatch thread（store）、consumer put_frame path（fetch_sub） | 同上 | `_Atomic uint32_t`；store 用 `memory_order_release`；fetch_sub 用 `memory_order_acq_rel` |
| `slot->state` | dispatch thread（FREE→IN_FLIGHT）、最後 consumer（IN_FLIGHT→FREE） | dispatch thread | `_Atomic slot_state_t`；IN_FLIGHT→FREE 用 CAS，防止 double-free |
| `slot->tombstone` | dispatch thread 的 `force_release`（寫） | consumer put_frame path（讀） | `_Atomic bool`；store `memory_order_release`；load `memory_order_acquire` |
| `session->refslot[]` / `refslot_count` | dispatch thread（唯一寫者） | dispatch thread | **不需要 lock**（單執行緒 server 假設）；跨 process 的 release msg 由 event loop 序列化處理 |
| `session->tombstone` | watchdog thread（寫） | dispatch thread（讀） | `_Atomic bool`；store `memory_order_release`；load `memory_order_acquire` |

### 7.3 關鍵 memory order 選擇理由

- `slot->refcount` 的 `store(release)`：確保 dispatch 在設定 refcount 前的所有 slot 寫入（frame_meta、state 轉 IN_FLIGHT）對後續 load(acquire) 的 consumer 可見
- `fetch_sub(acq_rel)`：確保 consumer 在遞減 refcount 時，看到的是最新的 tombstone 狀態；同時確保自己對 slot 的讀取在 refcount 歸零前完成
- `session->tombstone` 的 acquire/release pair：確保 watchdog 標記 tombstone 後，dispatch thread 看到時 session 的清理動作已完成

---

## 八、邊界情境與禁止行為

| 情境 | 正確處理 |
|------|---------|
| dispatch 時 consumer 剛斷線（socket 關閉） | `sendmsg` 回傳 `EPIPE`，從 dispatch list 移除此 consumer，refcount 不加 |
| `vfr_put_frame()` 時 slot 已被 force_release（DROP_OLDEST） | 檢查 `slot->tombstone` flag；若已標記，跳過 refcount 操作 |
| producer acquire 時無 FREE slot | 回傳 `EAGAIN`；若 policy 允許，producer 覆寫最舊的 READY slot |
| 同一個 consumer 連續兩次 `vfr_put_frame()` 同一 frame | 第二次 `dma_fd == -1`（已在第一次設為 -1），直接 return，不 close |
| `atomic_fetch_sub` 結果為 0 但 state 已是 FREE | 不重複標記，不重複通知；以 CAS 保護狀態轉移 |

---

*文件版本：v1.1 — 2026-04-22*  
*對應主計畫：plan-vfr-cross-platform-v2.md v2.3*  
*v1.1 變更：補 vfr_consumer_session_t 的 free_eventfd / session_id 欄位；修正 dispatch 偽碼 refcount race（Phase B/C 拆分）；補充 force_release 偽碼；新增 §五 IPC 握手雙向協議（vfr_client_hello_t / vfr_error_response_t）；新增 §六 跨 Process 回收協議（vfr_release_msg_t）；新增 §七 並發模型（前提假設 + 各欄位同步手段 + memory order 理由）；原 §五 邊界情境改為 §八*
