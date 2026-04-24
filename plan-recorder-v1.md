# VFR Recording Engine — 設計計畫書
# plan-recorder-v1.4.1.md

*基於 plan-vfr-cross-platform-v2.md v2.4 與 POOL_DESIGN.md v1.1*
*VFR 為幀來源層，Recording Engine 為其消費者模組之一*

---

## 一、專案目標與範圍

### 1.1 目標

實作一個工業級錄影核心引擎（`rec/`），以 VFR consumer 身分接收
Ambarella IAV 輸出的 **H264 / H265 encoded bitstream**，支援：

- **全天候錄影（24/7）**：持續寫入，按時間或大小自動分段
- **排程錄影（Scheduled）**：時間位元遮罩，以 15 分鐘為單位切換模式
- **事件錄影（Event）**：由 AI 跨 process 觸發，含預錄（Pre-record）與延時（Post-record）
- **預錄對齊 I-Frame**：觸發點回溯至最近 I-Frame，防止解碼綠屏
- **斷電可恢復**：採 MPEG-TS 容器，逐 packet 寫入；可恢復至最近一次 `fdatasync` 完成時的資料（page cache 中尚未落盤的部分可能丟失，見 §9.5）

### 1.2 不在範圍內

| 項目 | 說明 |
|------|------|
| 軟體編碼 | IAV HW encoder 輸出已編碼；本模組不呼叫 x264/x265 |
| 多路流合併 | 每個 `stream_name` 對應獨立 Recorder instance |
| 播放 / 回放 | 僅負責寫入，不實作 seek / playback |
| 網路傳輸 | RTSP 由獨立 consumer 負責 |
| 影像分析 | AI 觸發訊號由外部 AI process 送入，本模組不做分析 |

---

## 二、與 VFR 的接合點

### 2.1 VFR 修改項目（最小化）

Recording Engine 盡量不改動 VFR 核心。唯一需要新增的是：

**`include/vfr_defs.h`**：
```c
/* ─── Encoded Stream Flags（Phase 5+ Recorder 用）────────────────── */
#define VFR_FLAG_KEY_FRAME   (1u << 2)   /* I-Frame / IDR，Recorder 對齊起點 */

/* ─── Encoded Format fourcc ──────────────────────────────────────── */
#define VFR_FMT_H264   0x34363248u   /* 'H264' */
#define VFR_FMT_H265   0x35363248u   /* 'H265' / 'HEVC' */
```

**`ipc/vfr_ipc_types.h`**：新增 AI 觸發訊息型別（見 §五）。

**`platform/amba/amba_adapter.c`**：實作 IAV bitstream 讀取（見 §六）。

其餘 VFR 模組（`vfr_server`、`vfr_client`、`vfr_pool`、`vfr_sync`）**不需要改動**。

### 2.2 `vfr_frame_t` 欄位語意對照（Encoded 模式）

| 欄位 | Raw YUV 語意 | Encoded Bitstream 語意 |
|------|-------------|----------------------|
| `dma_fd` | YUV buffer fd | Bitstream buffer fd（memfd 或 IAV dma-buf）|
| `format` | `V4L2_PIX_FMT_NV12` | `VFR_FMT_H264` / `VFR_FMT_H265` |
| `buf_size` | YUV 總大小 | **Encoded frame 實際大小**（變長）|
| `flags` | `VFR_FLAG_NO_CPU_SYNC` | 新增 `VFR_FLAG_KEY_FRAME` |
| `stride` | luma pitch | 改放 `stream_id`（多路時識別）；存取必須透過 `REC_FRAME_STREAM_ID()` macro（見 rec_defs.h）|
| `plane_offset[3]` | Y/U/V 偏移 | 全部 reserved（填 0）|
| `timestamp_ns` | capture 時間 | IAV PTS 換算為 ns |
| `seq_num` | 幀序號 | 同上 |
| `width` / `height` | 像素解析度 | 編碼解析度（同） |

### 2.3 Backpressure Policy

Recorder 在 **VFR 層**使用 `VFR_BP_BLOCK_PRODUCER`（完整性優先）：
- 保護 VFR slot 不被 server 端強制回收
- 讓 VFR producer（IAV adapter）等待，而非丟失幀

**注意：VFR 層保幀 ≠ Write Queue 不丟幀。**
兩層語意必須分開理解：

| 層級 | 機制 | 溢出行為 | 說明 |
|------|------|----------|------|
| VFR consumer slot | `BLOCK_PRODUCER` | producer 等待 | 保護幀不被覆蓋，正常路徑 |
| Write Queue（內部） | SPSC bounded queue | `drop-until-next-IDR`，`drop_count++`（見 §9.6） | 磁碟 I/O 過載的最後防線 |

Write Queue 溢出屬於**磁碟 I/O 過載**情境（寫入速度跟不上攝影機輸出），此時 drop 是預期且可接受的行為。
Write Queue 溢出**不得** block event loop；應記錄 drop_count 供監控告警。

---

## 三、目錄結構

```text
vfr/
├── rec/                          ← 本計畫新增
│   ├── rec_defs.h                # 常數、型別、錯誤碼（唯一常數定義點）
│   ├── rec_buf.c / rec_buf.h     # Circular Byte Ring + Frame Index Table + pre_extract_queue
│   ├── rec_state.c / rec_state.h # 狀態機（IDLE/EXTRACT_PRE/WAIT_KEYFRAME/IN_EVENT/POST_WAIT）
│   ├── rec_schedule.c / rec_schedule.h  # 時間位元遮罩排程引擎
│   ├── rec_trigger.c / rec_trigger.h    # AI 觸發訊號接收（Unix socket）
│   ├── rec_debounce.c / rec_debounce.h  # 訊號消抖器
│   ├── rec_writer.c / rec_writer.h      # 異步寫入執行緒 + MPEG-TS 封裝
│   ├── rec_segment.c / rec_segment.h    # 檔案自動分段器
│   └── rec_engine.c / rec_engine.h      # 對外唯一 API（整合上述模組）
├── test/
│   ├── test_rec_buf.c            # Phase R1 驗收
│   ├── test_rec_state.c          # Phase R2 驗收
│   ├── test_rec_writer.c         # Phase R3 驗收
│   └── test_rec_full.c           # Phase R4 全流程驗收
└── Makefile                      # 新增 recorder targets
```

---

## 四、資料結構設計

### 4.1 `rec_defs.h` — 常數定義

```c
/* ─── Buffer 配置 ────────────────────────────────────────────────── */
#define REC_BUF_SIZE_DEFAULT   (15 * 1024 * 1024)  /* 15 MB，4K H265 約 10s */
#define REC_BUF_SIZE_MIN       (1  * 1024 * 1024)  /* 1 MB 下限 */
#define REC_BUF_SIZE_MAX       (64 * 1024 * 1024)  /* 64 MB 上限 */
#define REC_FRAME_INDEX_MAX    4096                 /* 最多追蹤 4096 幀 */

/* ─── 排程 ───────────────────────────────────────────────────────── */
#define REC_SCHEDULE_SLOTS_PER_DAY  96   /* 24h × 4（每 15 分鐘一格）*/

/* ─── 分段 ───────────────────────────────────────────────────────── */
#define REC_SEGMENT_DURATION_SEC  600    /* 預設每 10 分鐘分段 */
#define REC_SEGMENT_SIZE_MAX      (2LL * 1024 * 1024 * 1024)  /* 2 GB */

/* ─── 延時 ───────────────────────────────────────────────────────── */
#define REC_POST_RECORD_SEC_DEFAULT  30  /* 觸發消失後繼續錄影 30 秒 */
#define REC_DEBOUNCE_MS              500 /* 500 ms 內重複觸發視為同一事件 */

/* ─── epoll fd 上限 ──────────────────────────────────────────────── */
/* 已知 fd 來源：VFR eventfd(1) + AI trigger socket(1) + timerfd(1) */
#define REC_MAX_EPOLL_FDS  8             /* 保留 2x 彈性，caller buffer 至少此大小 */

/* ─── pre_extract_queue 容量 ─────────────────────────────────────── */
#define REC_PRE_QUEUE_DEPTH  REC_FRAME_INDEX_MAX

/* ─── write queue 容量 ───────────────────────────────────────────── */
#define REC_WRITE_QUEUE_DEPTH  256

/* ─── wrap-around 保護自旋逾時 ──────────────────────────────────── */
/*
 * 保護窗口持續時間 ≠ 單次 memcpy，而是整段 pre-roll drain 的總和。
 * 超時代表 Writer thread 已異常或長時間失去排程；此時 abort 本次 event clip。
 */
#define REC_PROTECT_SPIN_TIMEOUT_US  100000   /* 100 ms */

/* ─── ring 保護無效哨兵值 ────────────────────────────────────────── */
#define REC_PROTECT_NONE  UINT32_MAX
#define REC_PRE_GEN_NONE  0u

/* ─── stream_id accessor（stride 欄位語意重用）────────────────────── */
#define REC_FRAME_STREAM_ID(frame)         ((uint32_t)(frame)->stride)
#define REC_FRAME_SET_STREAM_ID(frame, id) ((frame)->stride = (uint32_t)(id))

/* ─── 錄影模式 ───────────────────────────────────────────────────── */
typedef enum {
    REC_MODE_CONTINUOUS = 0,
    REC_MODE_SCHEDULED  = 1,
    REC_MODE_EVENT      = 2,
} rec_mode_t;

/* ─── 狀態機狀態 ─────────────────────────────────────────────────── */
typedef enum {
    REC_STATE_IDLE          = 0,
    REC_STATE_EXTRACT_PRE   = 1,
    REC_STATE_WAIT_KEYFRAME = 2,
    REC_STATE_IN_EVENT      = 3,
    REC_STATE_POST_WAIT     = 4,
} rec_state_t;

/* ─── 排程格 ─────────────────────────────────────────────────────── */
typedef enum {
    REC_SLOT_OFF        = 0,
    REC_SLOT_CONTINUOUS = 1,
    REC_SLOT_EVENT      = 2,
} rec_slot_mode_t;

/* ─── 觸發事件 ───────────────────────────────────────────────────── */
typedef enum {
    REC_TRIGGER_START = 1,
    REC_TRIGGER_STOP  = 2,
} rec_trigger_type_t;

/* ─── Recorder 內部錯誤碼（節錄）────────────────────────────────── */
typedef enum {
    REC_OK = 0,
    REC_ERR_WRITER_STUCK = -1001,
    REC_ERR_PRE_ABORTED  = -1002,
    REC_ERR_QUEUE_FULL   = -1003,
} rec_error_t;
```

### 4.2 Circular Buffer、幀索引項、pre_extract_queue

```c
/* rec_buf.h */

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint64_t timestamp_ns;
    uint64_t seq_num;
    uint32_t batch_gen;     /* 0 = 非 pre-roll；>0 = 所屬 pre-roll batch */
    bool     is_keyframe;
} rec_frame_entry_t;

typedef struct {
    rec_frame_entry_t  entries[REC_PRE_QUEUE_DEPTH];
    _Atomic uint32_t   head;   /* Writer 讀取位置 */
    _Atomic uint32_t   tail;   /* event loop 寫入位置 */
} rec_pre_queue_t;

typedef struct rec_buf {
    uint8_t           *ring;
    uint32_t           ring_size;
    uint32_t           write_pos;

    rec_frame_entry_t  index[REC_FRAME_INDEX_MAX];
    uint32_t           index_head;
    uint32_t           index_tail;
    uint32_t           index_count;

    /*
     * protect window：Writer 讀 pre-roll 時，event loop 不得覆蓋此 byte 區間。
     * protected_gen = 此 protect window 的擁有 batch generation。
     * 只有擁有者 batch 的 Writer 才能在完成後清除此保護，避免舊 batch 誤清新 batch 的保護。
     */
    _Atomic uint32_t   protected_read_offset;
    _Atomic uint32_t   protected_read_size;
    _Atomic uint32_t   protected_gen;       /* REC_PRE_GEN_NONE = 無保護 */

    /*
     * next_pre_gen：event loop 每次 EXTRACT_PRE 產生新的 batch id。
     * aborted_pre_gen：event loop timeout / 手動取消時標記被放棄的 batch；
     * Writer 在 copy 前後都需檢查，若匹配則丟棄，不得寫入 TS。
     */
    _Atomic uint32_t   next_pre_gen;
    _Atomic uint32_t   aborted_pre_gen;     /* REC_PRE_GEN_NONE = 無 abort */

    rec_pre_queue_t    pre_queue;
} rec_buf_t;
```

**Pre-roll batch 規則：**

- 每次 `REC_STATE_EXTRACT_PRE` 會分配一個新的 `batch_gen`
- 同一個 batch 的所有 `rec_frame_entry_t` 都帶相同 `batch_gen`
- `protected_gen` 與該 batch 綁定；只有同一 batch 的 Writer drain 完成後可清除
- 若 event loop timeout、schedule off、manual stop 或 destroy 取消 pre-roll，必須：
  - `atomic_store(aborted_pre_gen, batch_gen)`
  - 清除 protect window
  - State Machine 放棄本次 event
- Writer 對已 abort 的 `batch_gen`：
  - 可以 dequeue
  - 可以完成已開始的 memcpy
  - **不得寫入 TS muxer**
  - 必須將同 batch 殘留 entry 視為垃圾工作並丟棄

### 4.3 Write Queue 元素

```c
/* rec_writer.h */
typedef struct {
    uint8_t  *data;         /* malloc'd copy（不持有 VFR slot）*/
    uint32_t  size;
    uint64_t  timestamp_ns;
    bool      is_keyframe;
    bool      is_segment_boundary;
    bool      is_shutdown_sentinel; /* true = destroy 時的結束訊號，data = NULL */
} rec_write_item_t;

typedef struct {
    rec_write_item_t  items[REC_WRITE_QUEUE_DEPTH];
    _Atomic uint32_t  head;
    _Atomic uint32_t  tail;
} rec_write_queue_t;
```

**Write Queue 資料所有權規則（live frame path）：**

- event loop 做單幀 `malloc + memcpy`，立即 `vfr_put_frame()` 釋放 VFR slot
- 允許的是 **單幀 live copy**；禁止的是 EXTRACT_PRE 的**批次/多幀** memcpy
- `free(item->data)` 一律由 Writer thread 負責

### 4.4 Codec Config Cache（雙緩衝快照）

```c
/* rec_writer.h */

#define REC_CODEC_CONFIG_MAX_SIZE  512

typedef struct {
    uint8_t  data[REC_CODEC_CONFIG_MAX_SIZE];
    uint32_t size;          /* 0 = 尚未收到 */
    uint32_t format;        /* VFR_FMT_H264 or VFR_FMT_H265 */
} rec_codec_blob_t;

typedef struct {
    rec_codec_blob_t  slots[2];
    _Atomic uint32_t  active_slot;   /* 0 or 1 */
    _Atomic uint32_t  version;       /* 每次 publish 遞增 */
} rec_codec_config_t;
```

**更新規則：**

- event loop 只寫入「非 active」的 slot
- 完成整份 blob 後，再以 `release store` 切換 `active_slot`，最後遞增 `version`
- Writer 以 `acquire load(active_slot)` 取得快照，先複製到 thread-local 暫存，再拿去插入 segment 起點
- **不使用 `updated` 布林**：codec config 是「最新穩定快照」，可被每個新 segment / event clip 重複使用，不是一次性消耗品

---

## 五、AI 觸發訊號 IPC 協議

### 5.1 Socket 路徑

```text
\0/vfr/event/<stream_name>
```

Recorder process 在此 socket **listen**；AI process **connect 後發送訊息並關閉**（無長連線）。

### 5.2 訊息定義（新增至 `ipc/vfr_ipc_types.h`）

```c
typedef struct {
    uint32_t magic;
    uint32_t event_type;                  /* rec_trigger_type_t */
    uint64_t timestamp_ns;                /* CLOCK_MONOTONIC */
    float    confidence;                  /* 0.0 ~ 1.0 */
    char     stream_name[VFR_SOCKET_NAME_MAX];
    char     label[32];
} vfr_event_msg_t;
```

### 5.3 訊號消抖器邏輯

```text
收到 TRIGGER_START：
  - 若距上次 START < REC_DEBOUNCE_MS → 忽略
  - 否則 → 通知 State Machine

收到 TRIGGER_STOP：
  - 若距上次 START < REC_DEBOUNCE_MS → 忽略（避免毛刺）
  - 否則 → 啟動 POST_WAIT 倒數
```

---

## 六、Amba Platform Adapter 修改

`platform/amba/amba_adapter.c` 的 `get_frame()` 需從 IAV 讀取 bitstream：

### 6.1 IAV Bitstream 讀取流程

```text
IAV_IOC_QUERY_DESC（非阻塞）
    ↓ 成功
struct iav_framedesc *desc
    ↓
判斷 pic_type（IDR / I / P / B）→ 設定 VFR_FLAG_KEY_FRAME
判斷 stream_type（H264 / H265）→ 設定 format fourcc
    ↓
建立 memfd（memfd_create）
write(memfd, desc->data_addr, desc->size)   ← 一次複製
    ↓（若 IAV 提供 dma-buf export，可改為零複製）
lseek(memfd, 0, SEEK_SET)
    ↓
填入 vfr_frame_t，回傳
```

### 6.2 零複製進階路徑（視 IAV 版本而定）

Ambarella IAV5/6 的 bitstream buffer 以 `mmap` 方式共享給 userspace。
若 IAV 支援 `dma_buf_export`，可直接傳遞 dma-buf fd，省去 memfd 複製。
實作骨架預留此路徑，初期以 memfd 確保正確性。

---

## 七、狀態機設計（核心）

### 7.1 狀態轉移圖

```text
IDLE
  ├─ TRIGGER_START 且排程允許 → EXTRACT_PRE
  └─ 其他 → 維持 IDLE

EXTRACT_PRE
  ├─ 找到可用 keyframe，pre_queue 建立成功 → IN_EVENT
  ├─ 完全沒有 keyframe → WAIT_KEYFRAME
  ├─ schedule off / manual stop / destroy → IDLE
  └─ protect timeout / Writer 異常 → IDLE（abort event）

WAIT_KEYFRAME
  ├─ 收到首個 IDR → IN_EVENT（首幀標記 is_segment_boundary）
  ├─ TRIGGER_STOP → IDLE      /* 尚未真正開檔，不進 POST_WAIT */
  ├─ schedule off → IDLE
  ├─ manual stop → IDLE
  └─ destroy → IDLE

IN_EVENT
  ├─ TRIGGER_STOP → POST_WAIT
  ├─ schedule off / manual stop → POST_WAIT
  └─ Writer 致命錯誤 → IDLE

POST_WAIT
  ├─ 倒數歸零且無新觸發 → IDLE
  ├─ TRIGGER_START → IN_EVENT
  ├─ schedule off / manual stop → IDLE
  └─ destroy → IDLE
```

### 7.2 EXTRACT_PRE 對齊演算法

**設計原則：**
Event loop thread **只做 index 掃描與 entry 複製**，不執行多幀 memcpy。
實際的 byte 複製由 Writer thread 消費 `pre_extract_queue` 時從 ring 讀出。
Ring buf 以 `protected_*` 欄位保護預錄區間不被 wrap-around 覆蓋。

```text
─── Event loop thread（EXTRACT_PRE 進入時執行）──────────────

1. target_ns = now_ns - pre_sec * 1_000_000_000

2. 掃描 Index Table，找起始 keyframe
   - 優先找 timestamp >= target_ns 的第一個 keyframe
   - 找不到則回退到最老的 keyframe
   - 完全找不到 → state = WAIT_KEYFRAME，直接返回

3. 分配新的 pre-roll generation
   pre_gen = atomic_fetch_add_explicit(&ring->next_pre_gen, 1, memory_order_relaxed) + 1
   atomic_store_explicit(&ring->aborted_pre_gen, REC_PRE_GEN_NONE, memory_order_release)

4. 計算 protect window
   protect_start = index[start_idx].offset
   if (write_pos >= protect_start)
       protect_size = write_pos - protect_start
   else
       protect_size = ring_size - protect_start + write_pos

5. 設定保護（先 size，再 offset，最後 owner generation）
   atomic_store_explicit(&ring->protected_read_size,   protect_size,  memory_order_release)
   atomic_store_explicit(&ring->protected_read_offset, protect_start, memory_order_release)
   atomic_store_explicit(&ring->protected_gen,         pre_gen,       memory_order_release)

6. 將 [start_idx .. index_tail) 的 index entry 複製進 pre_queue
   for each entry:
       tmp = index[i]
       tmp.batch_gen = pre_gen
       pre_queue_enqueue(&ring->pre_queue, &tmp)

7. 喚醒 Writer
   write(writer_eventfd, &one, sizeof(one))

8. 立即轉移至 IN_EVENT
```

**Abort 規則：**

```text
若 rec_buf_push() 在 protect overlap 上自旋超時：
  1. atomic_store(aborted_pre_gen, pre_gen)
  2. atomic_store(protected_gen, REC_PRE_GEN_NONE)
  3. atomic_store(protected_read_offset, REC_PROTECT_NONE)
  4. atomic_store(protected_read_size, 0)
  5. rec_state_force_idle(eng)
  6. 回傳 REC_ERR_WRITER_STUCK

Writer thread 開始 drain 一個 batch 前，先對 aborted_pre_gen 做一次 snapshot：
  abort_snapshot = atomic_load_explicit(&ring->aborted_pre_gen, memory_order_acquire)

整個 batch drain 期間用 abort_snapshot 判斷（不即時重讀），原因：
  雙重 abort 情境下，第二次 abort 會覆蓋 aborted_pre_gen，
  若 Writer 即時讀取，舊 batch（gen=5）在 aborted_pre_gen 被改成 gen=6 後就漏判，
  可能將已 protect-cleared 的髒資料寫入 TS。

Writer thread 在 dequeue 每個 pre entry 時：
  - copy 前先比對 entry.batch_gen == abort_snapshot
  - copy 完再比對一次
  - 若匹配：
      free(tmp_buf)
      不得 rec_segment_write()
      繼續丟棄同 batch 殘留工作
```

### 7.3 Writer thread 的雙來源優先序

```text
Writer 有兩個輸入來源，優先序固定：
  (A) pre_extract_queue
  (B) rec_write_queue

Writer loop：
  while running:
      wait writer_eventfd
      先 drain 所有 pre_extract_queue
      再 drain rec_write_queue

核心原則：
  - Writer 不得把 pre_queue 資料 re-enqueue 到 Write Queue
  - pre-roll 幀必須永遠早於 live 幀寫入檔案
  - pre_queue 與 live queue 共用同一個 writer_eventfd 喚醒
```

### 7.4 `rec_buf_overlaps()`（完整環形區間判斷）

**禁止使用單邊展開的簡化版。**
正確作法是：兩個環形區間都先展開成至多兩段線性區間，再做 `2 x 2` 線性 overlap 比較。

```c
typedef struct {
    uint32_t off;
    uint32_t size;
} rec_span_t;

static int rec_buf_split_span(uint32_t off, uint32_t size,
                              uint32_t ring_size,
                              rec_span_t spans[2])
{
    uint32_t end = off + size;
    if (size == 0)
        return 0;

    if (end <= ring_size) {
        spans[0].off  = off;
        spans[0].size = size;
        return 1;
    }

    spans[0].off  = off;
    spans[0].size = ring_size - off;
    spans[1].off  = 0;
    spans[1].size = end - ring_size;
    return 2;
}

static bool rec_linear_overlap(rec_span_t a, rec_span_t b)
{
    return (a.off < b.off + b.size) && (b.off < a.off + a.size);
}

static bool rec_buf_overlaps(uint32_t a_off, uint32_t a_size,
                             uint32_t b_off, uint32_t b_size,
                             uint32_t ring_size)
{
    rec_span_t a_spans[2];
    rec_span_t b_spans[2];
    int a_n = rec_buf_split_span(a_off, a_size, ring_size, a_spans);
    int b_n = rec_buf_split_span(b_off, b_size, ring_size, b_spans);

    for (int i = 0; i < a_n; ++i) {
        for (int j = 0; j < b_n; ++j) {
            if (rec_linear_overlap(a_spans[i], b_spans[j]))
                return true;
        }
    }
    return false;
}
```

### 7.5 `rec_buf_push()` 的保護等待

```text
1. 讀取 protected_read_offset / size / protected_gen（acquire）
2. 若無保護，正常寫入
3. 若有保護且新幀 byte range 與保護區間重疊：
   - 自旋等待，直到不重疊或保護解除
   - 超過 REC_PROTECT_SPIN_TIMEOUT_US：
       a. 標記 aborted_pre_gen = protected_gen
       b. 清除 protect window
       c. rec_state_force_idle()
       d. return REC_ERR_WRITER_STUCK
```

---

## 八、排程引擎設計

### 8.1 時間位元遮罩

```c
typedef struct {
    uint8_t slots[24];   /* 96 slots × 2 bits = 24 bytes */
} rec_schedule_day_t;

typedef struct {
    rec_schedule_day_t days[7];   /* 0 = Sunday */
} rec_schedule_t;
```

### 8.2 查詢當前應執行的模式

```c
rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched,
                                   time_t now_wall_clock)
{
    struct tm t;
    localtime_r(&now_wall_clock, &t);
    int slot = (t.tm_hour * 60 + t.tm_min) / 15;
    int bit_offset = slot * 2;
    uint8_t byte = sched->days[t.tm_wday].slots[bit_offset / 8];
    return (rec_slot_mode_t)((byte >> (bit_offset % 8)) & 0x3);
}
```

### 8.3 計時器驅動

排程邊界切換與 POST_WAIT 倒數共用同一個 **1 秒週期 `timerfd`**。

```c
uint64_t expirations;
read(timer_fd, &expirations, sizeof(expirations));

if (state == REC_STATE_POST_WAIT) {
    post_remaining_sec -= (int)expirations;
    if (post_remaining_sec <= 0 && !pending_trigger)
        rec_state_transition(eng, REC_STATE_IDLE);
}

time_t now = time(NULL);
rec_slot_mode_t slot = rec_schedule_query(&eng->config.schedule, now);
if (slot != eng->current_slot) {
    eng->current_slot = slot;
    rec_apply_schedule_change(eng, slot);
}
```

---

## 九、異步寫入引擎

### 9.1 設計原則

- `rec_write_queue`
  - **唯一 Producer = event loop thread**
  - Consumer = Writer thread
- `pre_extract_queue`
  - Producer = event loop thread
  - Consumer = Writer thread
- 兩者都由 **同一個 `writer_eventfd`** 喚醒 Writer
- Writer thread 有兩個輸入來源，優先序固定：
  1. `pre_extract_queue`
  2. `rec_write_queue`
- Writer 不得向 Write Queue enqueue 任何資料
- Writer thread 負責：封裝為 MPEG-TS → `write()` → `fdatasync()` → 分段判斷

### 9.2 Writer 喚醒規則

```text
以下情況都必須 write(writer_eventfd, 1)：
  1. EXTRACT_PRE 將一整批 pre-roll entry enqueue 完成後
  2. live path 成功 enqueue 新 frame 後
  3. destroy 前送入 shutdown sentinel 後

不允許「靠輪詢等 pre_queue 非空」。
原因：若 Writer 睡在 eventfd 上，而 pre_queue 沒有明確 wakeup，pre-roll 可能延遲到下一個 live frame 才開始處理。
```

### 9.3 MPEG-TS 封裝

```text
每個 encoded frame → 切割為 188-byte TS packets
Header：sync byte(0x47) + PID + continuity counter + adaptation field（PCR）
Payload：PES header（含 PTS）+ NAL data

新 segment / event clip 起點輸出順序：
  1. PAT
  2. PMT
  3. Codec Config NALUs（來自 rec_codec_config_t snapshot）
     - H.264：SPS + PPS
     - H.265：VPS + SPS + PPS
  4. IDR slice

若 cache 為空：
  - 不得從無 config 的 IDR 開新檔
  - 需延後 segment / event clip 開始，直到拿到第一份可用 codec config snapshot
```

### 9.4 檔案命名規則

```text
<output_dir>/<stream_name>_<YYYYMMDD>_<HHMMSS>_<mode>.ts
例：
  cam0_20260424_143000_cont.ts
  cam0_20260424_150230_event.ts
```

### 9.5 `fdatasync` 策略

```text
Recorder 在以下時機呼叫 fdatasync(fd)：
  1. 每個 segment 關閉前
  2. event clip 結束前（POST_WAIT → IDLE）
  3. 週期性 flush：flush_interval_sec（預設 60s，0 = 停用）

可恢復語意：
  - 可播放至最近一次 fdatasync 完成的資料
  - page cache 中尚未 sync 的資料可能丟失
```

### 9.6 Write Queue 溢出：drop-until-IDR 策略

```text
溢出時：
  1. eng->overflow_drop = true
  2. drop_count++
  3. 後續每幀都 drop，直到遇到下一個 IDR

收到 IDR 且 overflow_drop == true：
  1. overflow_drop = false
  2. 該 IDR 設 is_segment_boundary = true
  3. 正常 enqueue
```

---

## 十、對外 API（`rec_engine.h`）

```c
typedef struct rec_engine rec_engine_t;

typedef struct {
    char              stream_name[VFR_SOCKET_NAME_MAX];
    rec_mode_t        default_mode;
    rec_schedule_t    schedule;
    uint32_t          pre_record_sec;
    uint32_t          post_record_sec;
    uint32_t          segment_duration_sec;
    uint64_t          segment_size_max;
    uint32_t          ring_buf_size;
    uint32_t          flush_interval_sec;
    char              output_dir[256];
} rec_config_t;

rec_engine_t *rec_engine_create(const rec_config_t *cfg);
int  rec_engine_get_epoll_fds(rec_engine_t *eng, int *fds_out, int max_fds);
int  rec_engine_handle_event(rec_engine_t *eng, int fd);
void rec_engine_destroy(rec_engine_t **eng);

rec_state_t rec_engine_get_state(const rec_engine_t *eng);
uint64_t    rec_engine_get_written_bytes(const rec_engine_t *eng);
uint32_t    rec_engine_get_dropped_frames(const rec_engine_t *eng);
```

**epoll 所有權：由呼叫者全權管理。**

- 呼叫者負責 `EPOLL_CTL_ADD`
- 呼叫者負責 `EPOLL_CTL_DEL`
- `rec_engine_destroy()` 不操作 epoll

### 10.1 `rec_engine_destroy()` 資源釋放順序

```text
前置條件：
  呼叫者必須先對 rec_engine_get_epoll_fds() 回傳的所有 fd 執行 EPOLL_CTL_DEL

1. enqueue shutdown sentinel 至 Write Queue
2. write(writer_eventfd, 1)
3. pthread_join(writer_thread)
4. close(writer_eventfd)
5. close(timer_fd)
6. close(trigger_socket_fd)
7. vfr_close(&vfr_ctx)
8. free(ring->ring)
9. free(rec_buf)
10. free(rec_engine)
11. *eng = NULL
```

---

## 十一、開發階段（MVP Roadmap）

### Phase R1：Circular Buffer + I-Frame Indexer

**目標**：底層幀緩衝正確性驗證，含 protect 機制與 batch abort

實作項目：
- `rec_buf.c`：byte-ring 寫入、wrap-around 淘汰、`rec_buf_overlaps()`
- `rec_buf.h`：`rec_buf_push()`、`rec_buf_extract_from_keyframe()`、`rec_pre_queue_t`
- `test/test_rec_buf.c`

驗收 Checklist：
- [ ] 單元測試：寫入 1000 幀，buffer 滿後舊幀自動淘汰，`index_head` 正確推進
- [ ] I-Frame 定位：`extract_from_keyframe(t_now - 10s)` 回傳正確起始 entry
- [ ] Wrap-around：buffer 環繞後 index offset 計算正確，無越界
- [ ] `rec_buf_overlaps()` 單元測試：非環繞、單邊環繞、雙邊環繞各場景
- [ ] protect window 擁有者驗證：舊 batch 不可清除新 batch 的 protect
- [ ] batch abort：timeout 後同 batch 殘留 entry 被 Writer 丟棄，不寫入 TS
- [ ] ASan / valgrind：無記憶體洩漏、無越界讀寫

### Phase R2：狀態機（無寫入，僅 Log）

**目標**：驗證五狀態轉移邏輯，不依賴 I/O

實作項目：
- `rec_state.c`：五狀態轉移 + 消抖器 + POST_WAIT 倒數
- `rec_debounce.c`：時間過濾
- `rec_trigger.c`：Unix socket listener
- `test/test_rec_state.c`

驗收 Checklist：
- [ ] IDLE → EXTRACT_PRE → IN_EVENT：觸發後狀態正確轉移
- [ ] EXTRACT_PRE → WAIT_KEYFRAME：無可用 keyframe 時進入等待
- [ ] WAIT_KEYFRAME + IDR → IN_EVENT：首個 IDR 到來後開始錄影
- [ ] WAIT_KEYFRAME + STOP / schedule off / manual stop → IDLE
- [ ] POST_WAIT → IN_EVENT：延時期間收到新觸發，正確回歸
- [ ] POST_WAIT → IDLE：倒數結束後無新觸發，回到待命
- [ ] 排程關閉時段內，`TRIGGER_START` 被靜默忽略
- [ ] timerfd 驅動倒數，正確消耗 expiration count

### Phase R3：異步寫入 + 分段

**目標**：實際產生可播放的 `.ts` 檔案

實作項目：
- `rec_writer.c`：Write Queue + pre_extract_queue 消費 + Writer thread + MPEG-TS 封裝
- `rec_segment.c`：按時間 / 大小切檔、`is_segment_boundary` 旗標設定
- `test/test_rec_writer.c`

驗收 Checklist：
- [ ] 產生的 `.ts` 可用 `ffprobe` 正確讀取 duration、codec、PTS
- [ ] 分段切換：新 segment 從 `PAT + PMT + codec config + IDR` 開始
- [ ] codec config snapshot：Writer 讀到一致快照，不讀半包資料
- [ ] pre_extract_queue 消費：Writer 正確從 ring 讀出預錄段
- [ ] aborted batch：Writer 丟棄殘留 pre entry，不寫入壞資料
- [ ] Write Queue 溢出：`drop_count` 遞增，不 crash，且採 `drop-until-IDR`
- [ ] destroy 流程：sentinel 送入後 Writer 正常排空並結束
- [ ] ASan：無記憶體錯誤

### Phase R4：排程 + 全流程整合

**目標**：接上真實 VFR，在 Amba 平台驗證完整流程

實作項目：
- `rec_schedule.c`
- `rec_engine.c`
- `platform/amba/amba_adapter.c`
- `test/test_rec_full.c`

驗收 Checklist：
- [ ] 24/7 模式：持續錄影 1 小時，每 10 分鐘自動分段
- [ ] 事件模式：AI trigger → 預錄對齊 I-Frame → 錄影 → POST_WAIT → 結束
- [ ] 排程模式：時段 off → 無輸出；時段 continuous → 正常錄影
- [ ] 跨 process 觸發：AI process 送 `vfr_event_msg_t`，Recorder 正確響應
- [ ] `rec_engine_destroy()` 順序驗證：無 leak，fd 計數恢復
- [ ] Prometheus metrics：`rec_drop_frames`、`rec_written_bytes`、`rec_state` 可 scrape

---

## 十二、嚴格禁止事項

| 禁止行為 | 原因 |
|----------|------|
| 在 event loop thread 呼叫 `write()` | 磁碟 I/O 阻塞，所有 VFR 幀事件卡死 |
| 在 event loop thread 呼叫 `sleep()` / `usleep()` | 同上 |
| 在 event loop thread 對大型 buffer 執行**批次/多幀連續 `memcpy()`** | pre-roll memcpy 必須由 Writer thread 執行 |
| Writer thread 直接讀取 ring buf 不設 protect window | 可能讀到被 wrap 覆蓋的資料 |
| Writer 在 dequeue 後立即清除 protect | memcpy 尚未完成，event loop 可能提前覆蓋 |
| 使用單邊展開版 `rec_buf_overlaps()` | 在雙邊環繞情境會誤判 |
| protect timeout 後繼續覆蓋保護區間 | 會破壞尚未讀出的 pre-roll |
| Writer 將 pre_queue 資料 re-enqueue 至 Write Queue | 打破單一 producer，且會破壞時序 |
| pre_queue enqueue 後不喚醒 Writer | pre-roll 可能延遲到下一個 live frame 才處理 |
| Write Queue 溢出時任意丟幀 | 解碼器會在非 IDR 邊界花屏 |
| segment 起點不輸出 codec config NALUs | 大多播放器無法獨立解碼 |
| 使用 consume-once 的 `codec_config.updated` 布林 | 後續新 segment 會缺 config；cache 應可重複使用 |
| destroy 前未 `EPOLL_CTL_DEL` 所有 engine fd | epoll 可能持有 dangling fd |

---

## 十三、關鍵設計決策表

| 項目 | 決策 | 原因 |
|------|------|------|
| 容器格式 | MPEG-TS | 斷電恢復友善，無需 moov atom |
| Circular Buffer 型態 | Byte-ring + Frame Index Table | Encoded frame 變長 |
| 預錄資料來源 | Writer 從 ring 自行 memcpy | 不持有 VFR slot，event loop 不做批次 copy |
| Writer 輸入優先序 | `pre_queue` 先於 `write_queue` | 保證 pre-roll 永遠先於 live |
| Pre-roll 取消機制 | `batch_gen + aborted_pre_gen + protected_gen` | 避免 timeout 後殘留工作污染下一段 clip |
| protect owner | `protected_gen` 擁有權綁 batch | 舊 batch 不會誤清新 batch 保護 |
| overlap 判斷 | 雙方都展開為最多兩段線性區間 | wrap case 可正確實作 |
| 自旋逾時 | `REC_PROTECT_SPIN_TIMEOUT_US = 100000` | 100ms 才足以涵蓋整段 pre-roll drain |
| codec config cache | 雙緩衝快照 + version | Writer 不會讀到半更新資料，且可重複使用 |
| Writer 喚醒 | 共用 `writer_eventfd`，pre/live/sentinel 都要喚醒 | 不靠輪詢，無延遲死角 |
| 無 keyframe fallback | 顯式 `WAIT_KEYFRAME` 狀態 | 不允許 clip 從 P-frame 開始 |
| Write Queue 丟幀策略 | `drop-until-next-IDR` | 從完整 GOP 邊界恢復，避免花屏 |
| `fdatasync` 節奏 | segment close + event clip end + 可選週期 flush | 讓恢復語意可測量 |
| epoll 所有權 | 完全由 caller 管理 | engine 不持有 epoll fd |

---

## 十四、相依套件

| 套件 | 用途 | 最低版本 | 必要性 |
|------|------|----------|--------|
| Linux kernel | `eventfd`、`memfd`、`timerfd`、`futex` | 5.4+ | 必要 |
| VFR（本專案） | 幀來源 | Phase 5 完成版 | 必要 |
| Ambarella IAV | HW encode bitstream | IAV5 / IAV6 | Amba 平台必要 |
| pthreads | Writer thread | glibc | 必要 |
| *(自實作 TS muxer)* | MPEG-TS 封裝 | — | 無外部依賴 |

---

## 十五、Build 系統

```makefile
SRCS_REC = \
    rec/rec_buf.c \
    rec/rec_state.c \
    rec/rec_schedule.c \
    rec/rec_trigger.c \
    rec/rec_debounce.c \
    rec/rec_writer.c \
    rec/rec_segment.c \
    rec/rec_engine.c

test_rec_buf: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) \
              rec/rec_buf.c test/test_rec_buf.c
	$(CC) $(CFLAGS) $(INCLUDES) -Irec -o $@ $^

test_rec_full: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) \
               $(SRCS_IPC_SERVER) $(SRCS_REC) test/test_rec_full.c
	$(CC) $(CFLAGS) $(INCLUDES) -Irec -o $@ $^ -lpthread
```

---

*文件版本：v1.4.1 — 2026-04-24*
*v1.4.1 修正：2 個邊緣案例*
*  🟡 aborted_pre_gen 單值在雙重 abort 時可能漏判：Writer 改用 batch 開始時的 snapshot 判斷*
*  🟢 §2.3 Backpressure 表格 overflow 欄與 §9.6 不一致（改為 drop-until-next-IDR）*
*v1.4 修正：6 個剩餘實作歧義*
*  🔴 pre-roll abort 後殘留 queue 工作未定義（新增 batch_gen / aborted_pre_gen / protected_gen）*
*  🔴 codec config cache data race（改為雙緩衝快照 + version；移除 updated bool）*
*  🔴 rec_buf_overlaps() 單邊展開不正確（改為雙方 split 成最多兩段後做 2x2 overlap）*
*  🟡 Writer 對 pre_queue 的喚醒語意不明（定死共用 writer_eventfd，禁止輪詢）*
*  🟡 WAIT_KEYFRAME 缺少 STOP / schedule off / manual stop 轉移（補完整轉移表）*
*  🟢 決策表 timeout 數值殘留 2000us（統一為 100000us）*
*v1.3 修正：9 個設計問題*
*v1.2 修正：6 個設計問題*
*v1.1 修正：10 個設計問題*

