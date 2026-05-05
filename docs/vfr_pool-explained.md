# vfr_pool 實作說明

## 概述

`vfr_pool` 是整個系統的 **Buffer Slot 管理器**，負責追蹤每一個 DMA frame buffer 的生命週期。
採用 lock-free 設計，用 atomic CAS 做狀態轉換、atomic fetch_sub 做 refcount，確保多 consumer 場景下 buffer 的安全回收。

---

## 核心資料結構

```
struct vfr_pool
├── slot_count
├── platform (ops + ctx)
└── islots[]  ← flexible array，每個 slot 是一個 vfr_islot_t
```

### `vfr_islot_t` — 單一 slot 的內部狀態

| 欄位 | 說明 |
|------|------|
| `state` | 這個 slot 現在在哪個階段：空閒 / 正在填資料 / 準備好 / 正在被用 |
| `refcount` | 還有幾個 consumer 正在用這個 buffer，降到 0 才能回收 |
| `tombstone` | 被強制回收時插的旗子，讓遲到的 release 知道「已經死了，不用管」 |
| `frame_meta` | 這個 buffer 的實際內容：DMA fd、影像尺寸、timestamp 等 metadata |
| `ref` | 反向指標（back-reference）—— consumer 透過它找回「我屬於哪個 pool、哪個 slot」 |

### `struct vfr_pool` — Pool 本身

| 欄位 | 說明 |
|------|------|
| `slot_count` | 總共有幾個 slot |
| `platform` | 平台 ops 函式表（如何向硬體拿幀、還幀），借用指標不擁有 |
| `platform_ctx` | 平台 adapter 的私有狀態，由 `platform->init()` 設定 |
| `islots[]` | 所有 slot 的陣列，緊接在 struct 後面（C flexible array），`calloc` 一次分配完畢 |

---

## 為什麼需要反向指標（back-reference）？

Consumer 拿到的只是 `vfr_frame_t`，**不知道自己從哪個 pool 的哪個 slot 來的**。

**沒有反向指標時**，consumer 必須自己記住：
```c
vfr_put_frame(pool, slot_idx, frame);  // 要額外傳 pool 和 slot_idx
```

**有反向指標後**，只需要：
```c
vfr_put_frame(frame);  // frame 裡面已經知道一切
```

內部實作：
```c
vfr_slot_ref_t *ref = frame->priv;   // 取出反向指標
struct vfr_pool *p  = ref->pool;     // 找到 pool
uint32_t idx        = ref->slot_idx; // 找到 slot
```

> `frame` 被 dispatch 出去後就脫離了 pool 的掌控，反向指標讓它「帶著回家的地址」，無論被傳到哪裡，都能自己找到路回來。

---

## Slot 狀態機

```
FREE
 │  acquire()
 ▼
FILLING
 │  get_frame() 成功
 ▼
READY
 │  dispatch / begin_dispatch
 ▼
IN_FLIGHT ── consumer 使用中（refcount > 0）
 │  最後一個 put / server_release / force_release
 ▼
FREE
```

---

## `dispatch_single` vs `begin_dispatch`

| | `dispatch_single` | `begin_dispatch` |
|---|---|---|
| 使用場景 | 同 process（Phase 1） | 跨 process IPC（Phase 2） |
| fd 傳遞方式 | `dup()` | `sendmsg` SCM_RIGHTS |
| consumer 數量 | 固定 1 | 動態 n 個 |
| 完成時機 | 呼叫後立即可用 | 呼叫後還需要逐一 `sendmsg` |

- **`dispatch_single`**：「自己 dup 一份給自己」
- **`begin_dispatch`**：「先登記好幾個人要用，再一個一個透過 socket 傳出去」

---

## 完整使用流程

### Phase 1 — 同 process（單 consumer）

```
┌─────────────────────────────────────────────────────┐
│  1. vfr_pool_create()                               │
│     建立 pool，初始化所有 slot 為 FREE               │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│  2. vfr_pool_acquire()                              │
│     CAS 搶一個 FREE slot → FILLING                  │
│     呼叫 platform->get_frame() 填入 fd + metadata   │
│     → READY，回傳 slot_idx                          │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│  3. vfr_pool_dispatch_single()                      │
│     dup(producer_fd) → consumer_fd                  │
│     refcount = 1，slot → IN_FLIGHT                  │
│     out_frame 帶著 consumer_fd + back-ref 交給 consumer │
└──────────────────┬──────────────────────────────────┘
                   │  consumer 使用 frame...
┌──────────────────▼──────────────────────────────────┐
│  4. vfr_put_frame()  →  vfr_pool_put_slot()         │
│     close consumer_fd                               │
│     refcount 遞減，降為 0                           │
│     → platform->put_frame()，slot → FREE            │
└─────────────────────────────────────────────────────┘
```

### Phase 2 — 跨 process（多 consumer）

```
┌─────────────────────────────────────────────────────┐
│  1. vfr_pool_create()  （同上）                     │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│  2. vfr_pool_acquire()  （同上）                    │
│     → READY，回傳 slot_idx                          │
└──────────────────┬──────────────────────────────────┘
                   │
                   │ 沒有 consumer 連線？
                   ├─ YES ──→ vfr_pool_cancel_acquire()
                   │          直接 FREE，結束
                   │
┌──────────────────▼──────────────────────────────────┐
│  3. vfr_pool_begin_dispatch(slot_idx, n_consumers)  │
│     refcount = n_consumers，slot → IN_FLIGHT        │
│     ★ 必須在 sendmsg 之前完成                       │
└──────────────────┬──────────────────────────────────┘
                   │
┌──────────────────▼──────────────────────────────────┐
│  4. sendmsg(SCM_RIGHTS) × n_consumers               │
│     kernel 自動複製 fd 給每個 consumer process      │
└──────────────────┬──────────────────────────────────┘
                   │  各 consumer 用完後送 release msg
┌──────────────────▼──────────────────────────────────┐
│  5. vfr_pool_server_release()  （每個 consumer 各一次）│
│     驗證 seq_num                                    │
│     refcount 遞減；最後一個降為 0                   │
│     → platform->put_frame()，slot → FREE            │
└─────────────────────────────────────────────────────┘
```

### 特殊情況：DROP_OLDEST（Phase 3）

當 pool 滿了、新幀進不來，強制回收最舊的 IN_FLIGHT slot：

```
vfr_pool_force_release()
  → refcount 遞減
  → 若降為 0：立即回收
  → 若還有其他 consumer：等他們的 server_release 處理完最後清理
```

---

## fd 生命週期

```
producer side:  slot->frame_meta.dma_fd  （pool 持有，platform->put_frame 負責 close）
consumer side:  out_frame->dma_fd        （dup'd fd，vfr_put_frame 負責 close）
```

兩端指向同一個底層 memfd/dma_buf，close 互不影響。
Phase 1 的 `dup()` 行為與 Phase 2 的 `SCM_RIGHTS` 語意完全對齊。
