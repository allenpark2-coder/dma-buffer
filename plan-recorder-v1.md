# VFR Recording Engine — 設計計畫書
# plan-recorder-v1.3.md

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
| Write Queue（內部） | SPSC bounded queue | `drop_count++`，不 crash | 磁碟 I/O 過載的最後防線 |

Write Queue 溢出屬於**磁碟 I/O 過載**情境（寫入速度跟不上攝影機輸出），此時 drop 是預期且可接受的行為。
Write Queue 溢出**不得** block event loop；應記錄 drop_count 供監控告警。

---

## 三、目錄結構

```
vfr/
├── rec/                          ← 本計畫新增
│   ├── rec_defs.h                # 常數、型別、錯誤碼（唯一常數定義點）
│   ├── rec_buf.c / rec_buf.h     # Circular Byte Ring + Frame Index Table + pre_extract_queue
│   ├── rec_state.c / rec_state.h # 狀態機（IDLE/EXTRACT_PRE/IN_EVENT/POST_WAIT）
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
/*
 * 預錄 index entry 暫存佇列深度。
 * 上限 = REC_FRAME_INDEX_MAX，保證 EXTRACT_PRE 可將全部 index entry 一次送入。
 * 此 queue 為 SPSC（event loop 寫，Writer thread 讀），用 atomic head/tail 實作，無 mutex。
 */
#define REC_PRE_QUEUE_DEPTH  REC_FRAME_INDEX_MAX   /* = 4096 */

/* ─── wrap-around 保護自旋逾時 ──────────────────────────────────── */
/*
 * rec_buf_push() 等待 protected_read_offset 清除的最大自旋時間（微秒）。
 * 超過此值代表 Writer thread 卡住或死亡，abort 本次 event clip 並 log ERROR。
 *
 * 預設 100000 µs（100ms）：
 *   保護窗口持續時間 ≠ 單次 memcpy，而是整段 pre-roll drain 的總和：
 *   最壞情況 = REC_BUF_SIZE_MAX (64 MB) / DDR 2 GB/s = ~32ms + malloc/enqueue 開銷
 *   100ms 給 3x margin；超過此值代表 Writer 已異常，繼續等待無意義。
 *
 * 注意：逾時後「不得」繼續寫入覆蓋保護區間；應 abort 本次 event 並清除保護旗標。
 */
#define REC_PROTECT_SPIN_TIMEOUT_US  100000

/* ─── ring 保護無效哨兵值 ────────────────────────────────────────── */
#define REC_PROTECT_NONE  UINT32_MAX

/* ─── stream_id accessor（stride 欄位語意重用）────────────────────── */
/*
 * vfr_frame_t.stride 在 encoded 模式下存放 stream_id。
 * 使用 macro 存取，避免 implicit reuse 造成混淆。
 * 未來如 VFR 新增專用欄位，改此 macro 即可，呼叫端不動。
 */
#define REC_FRAME_STREAM_ID(frame)         ((uint32_t)(frame)->stride)
#define REC_FRAME_SET_STREAM_ID(frame, id) ((frame)->stride = (uint32_t)(id))

/* ─── 錄影模式 ───────────────────────────────────────────────────── */
typedef enum {
    REC_MODE_CONTINUOUS = 0,  /* 24/7 連續錄影 */
    REC_MODE_SCHEDULED  = 1,  /* 依排程位元遮罩 */
    REC_MODE_EVENT      = 2,  /* 純事件錄影（無觸發則不錄）*/
} rec_mode_t;

/* ─── 狀態機狀態 ─────────────────────────────────────────────────── */
typedef enum {
    REC_STATE_IDLE          = 0,  /* 待命：僅執行環形緩衝 */
    REC_STATE_EXTRACT_PRE   = 1,  /* 預錄提取：將 index entry 送入 pre_extract_queue */
    REC_STATE_WAIT_KEYFRAME = 2,  /* 等待首個 IDR：pre-roll 無可用 keyframe，暫存至 IDR 到來 */
    REC_STATE_IN_EVENT      = 3,  /* 錄影中：即時幀導向 Write Queue */
    REC_STATE_POST_WAIT     = 4,  /* 延時等待：倒數，可被新觸發打斷 */
} rec_state_t;

/* ─── 排程格 ─────────────────────────────────────────────────────── */
typedef enum {
    REC_SLOT_OFF        = 0,  /* 此時段不錄影 */
    REC_SLOT_CONTINUOUS = 1,  /* 此時段連續錄影 */
    REC_SLOT_EVENT      = 2,  /* 此時段事件錄影 */
} rec_slot_mode_t;

/* ─── 觸發事件 ───────────────────────────────────────────────────── */
typedef enum {
    REC_TRIGGER_START = 1,
    REC_TRIGGER_STOP  = 2,
} rec_trigger_type_t;
```

### 4.2 Circular Buffer、幀索引項、pre_extract_queue

```c
/* rec_buf.h */

/* 單一幀的 index entry（event loop 與 Writer 之間傳遞的基本單元） */
typedef struct {
    uint32_t offset;        /* 在 byte ring 中的起始偏移 */
    uint32_t size;          /* 此幀 encoded 大小（bytes）*/
    uint64_t timestamp_ns;
    uint64_t seq_num;
    bool     is_keyframe;
} rec_frame_entry_t;

/*
 * pre_extract_queue：EXTRACT_PRE 時，event loop 將預錄段的 index entry
 * 推入此 queue；Writer thread 從此 queue 讀出 entry，再到 ring buf 做 memcpy。
 *
 * 設計：SPSC bounded queue，atomic head/tail，無 mutex。
 * 深度：REC_PRE_QUEUE_DEPTH（= REC_FRAME_INDEX_MAX = 4096）。
 * 生命週期：屬於 rec_buf_t，與 ring buf 共用同一個 protect 機制。
 * 所有權：entry 為 value copy（非指標），Writer 讀完即丟，不需 free。
 */
typedef struct {
    rec_frame_entry_t  entries[REC_PRE_QUEUE_DEPTH];
    _Atomic uint32_t   head;   /* Writer 讀取位置 */
    _Atomic uint32_t   tail;   /* event loop 寫入位置 */
} rec_pre_queue_t;

typedef struct rec_buf {
    uint8_t           *ring;          /* 環形 byte 緩衝區（malloc）*/
    uint32_t           ring_size;     /* 總容量（bytes）*/
    uint32_t           write_pos;     /* 下一個寫入位置（僅 event loop 讀寫）*/

    rec_frame_entry_t  index[REC_FRAME_INDEX_MAX];
    uint32_t           index_head;   /* 最老的幀（僅 event loop 讀寫）*/
    uint32_t           index_tail;   /* 下一個寫入位置（僅 event loop 讀寫）*/
    uint32_t           index_count;  /* 目前幀數（僅 event loop 讀寫）*/

    /*
     * Writer thread 正在讀取的 ring 保護區間（Issue #1 修正）。
     *
     * event loop（EXTRACT_PRE）設定保護區間後，rec_buf_push() 在
     * wrap-around 前必須確認新幀的 byte range 不與保護區間重疊；
     * 若有重疊，自旋等待（上限 REC_PROTECT_SPIN_TIMEOUT_US µs）。
     *
     * Writer thread 在最後一個 pre_extract_queue entry 的 memcpy「完成後」
     * 清除保護（atomic_store UINT32_MAX），不是 dequeue 後立即清除。
     *
     * 兩個欄位為獨立 atomic，event loop 先寫 size 再寫 offset（release order）；
     * Writer 先讀 offset 再讀 size（acquire order），確保可見性。
     */
    _Atomic uint32_t   protected_read_offset; /* REC_PROTECT_NONE = UINT32_MAX */
    _Atomic uint32_t   protected_read_size;   /* 0 = 無保護 */

    rec_pre_queue_t    pre_queue;    /* EXTRACT_PRE 用，屬於 rec_buf_t */
} rec_buf_t;

/*
 * rec_buf_push() 內部淘汰規則（Index Table 與 byte ring 同步）：
 *
 * 在寫入新幀前，若新幀的 byte range [write_pos, write_pos + size) 會覆蓋
 * index[index_head] 的 [offset, offset + size) 範圍，
 * 則先推進 index_head（丟棄最老幀的 index entry），重複直到無重疊。
 * 此邏輯確保 index entry 的 offset 永遠指向有效（未被覆蓋）的 ring 資料。
 *
 * 若新幀 byte range 與 protected_read_offset 重疊，自旋等待（見上方說明）。
 */
```

### 4.3 Write Queue 元素

```c
/* rec_writer.h */
typedef struct {
    uint8_t  *data;         /* malloc'd copy（不持有 VFR slot）*/
    uint32_t  size;
    uint64_t  timestamp_ns;
    bool      is_keyframe;
    bool      is_segment_boundary;  /* Writer 看到此旗標時切換新檔案 */
                                    /* 由 rec_segment.c 在 enqueue 前設定 */
    bool      is_shutdown_sentinel; /* true = destroy 時的結束訊號，data = NULL */
} rec_write_item_t;
```

**Write Queue 資料所有權規則（live frame path）：**
- event loop 做單幀 malloc + memcpy（典型 ≤ 500 KB / ~250 µs），立即 vfr_put_frame() 釋放 VFR slot
- 單幀 copy 在 event loop 執行是預期且可接受的（§12 禁止的是「EXTRACT_PRE 批次/多幀 memcpy」）



| 動作 | 執行方 | 說明 |
|------|--------|------|
| `malloc(item->data)` | Producer（event loop） | enqueue 前分配，複製幀資料後送入 queue |
| `free(item->data)` | Writer thread | dequeue 後，`write()` 完成即釋放；sentinel item 的 data = NULL，不 free |
| destroy 時排空 queue | Writer thread 先執行 | 收到 sentinel 後，繼續排空 queue 並逐一 `free(data)`，然後 thread 自行結束 |
| `rec_engine_destroy()` 等待 | Event loop 端 | `pthread_join(writer_thread)` 確保 Writer 完全結束後，才釋放 `rec_buf_t` 及 `rec_engine_t` |

### 4.4 Codec Config Cache

```c
/* rec_buf.h — 最新 SPS/PPS/VPS 快取（event loop 更新，Writer 讀取）*/

#define REC_CODEC_CONFIG_MAX_SIZE  512   /* SPS+PPS（H.264）或 VPS+SPS+PPS（H.265）上限 */

typedef struct {
    uint8_t  data[REC_CODEC_CONFIG_MAX_SIZE];
    uint32_t size;          /* 0 = 尚未收到 */
    uint32_t format;        /* VFR_FMT_H264 or VFR_FMT_H265 */
    bool     updated;       /* event loop 寫入新 config 後置 true，Writer 讀完後清除 */
} rec_codec_config_t;
```

**更新規則：**
- Platform adapter（amba_adapter.c）在 IAV SPS/PPS NAL unit 到達時，填充 `rec_codec_config_t` 並通知 Recorder
- 或 Recorder 在每幀判斷：若 `is_keyframe == true` 且幀資料起始為 SPS/PPS NAL，更新 cache
- Writer 在每個 segment / event clip 起點，在 PAT+PMT 後、IDR TS packets 前，輸出 codec config NALUs

---

## 五、AI 觸發訊號 IPC 協議

### 5.1 Socket 路徑

```
\0/vfr/event/<stream_name>     ← abstract namespace，延續 VFR 慣例
```

Recorder process 在此 socket **listen**；AI process **connect 後發送訊息並關閉**（無長連線）。

### 5.2 訊息定義（新增至 `ipc/vfr_ipc_types.h`）

```c
/* AI → Recorder 觸發訊息 */
typedef struct {
    uint32_t magic;                       /* VFR_SHM_MAGIC 校驗 */
    uint32_t event_type;                  /* rec_trigger_type_t */
    uint64_t timestamp_ns;                /* 事件發生時間（CLOCK_MONOTONIC）*/
    float    confidence;                  /* AI 信心分數（0.0 ~ 1.0）*/
    char     stream_name[VFR_SOCKET_NAME_MAX];
    char     label[32];                   /* 事件標籤，e.g. "motion", "vehicle" */
} vfr_event_msg_t;
```

### 5.3 訊號消抖器邏輯

```
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

```
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

```
                    ┌──────────────────────────────────────────────┐
                    │ REC_STATE_IDLE                               │
                    │ 動作：每幀寫入 Circular Buffer               │
                    └──────────────────┬───────────────────────────┘
                                       │ TRIGGER_START 且
                                       │ 排程允許錄影
                                       ▼
                    ┌──────────────────────────────────────────────┐
                    │ REC_STATE_EXTRACT_PRE                        │
                    │ 動作（event loop，僅做 index 操作）：        │
                    │  1. 從 Index Table 找最近 I-Frame            │
                    │  2. 計算保護區間（含環繞修正）               │
                    │  3. 設定 protected_read_offset/size          │
                    │  4. 將 index entry 推入 pre_extract_queue    │
                    │     ★ 不在此做 memcpy，不阻塞 event loop ★  │
                    │  5a. 有 keyframe → 切換至 IN_EVENT          │
                    │  5b. 無 keyframe → 切換至 WAIT_KEYFRAME     │
                    └──────────────────┬───────────────────────────┘
                                       │ 提取完成（僅 index 操作，< 0.1ms）
                          ┌────────────┴────────────┐
                          ▼                         ▼
           ┌──────────────────────┐   ┌─────────────────────────────┐
           │ REC_STATE_           │   │ REC_STATE_WAIT_KEYFRAME     │
           │ WAIT_KEYFRAME        │   │（pre-roll 無 keyframe 可用）│
           │ 動作：丟棄幀直到     │──→│ 等首個 IDR 到來             │
           │ 首個 IDR 到達        │   │ → 設 is_segment_boundary    │
           └──────────┬───────────┘   │ → 切換至 IN_EVENT           │
                      │               └─────────────────────────────┘
                      │ 首個 IDR
                      ▼
                    ┌──────────────────────────────────────────────┐
                    │ REC_STATE_IN_EVENT                           │
                    │ 動作：每幀送入 Write Queue（event loop）     │
                    │       Writer 優先 drain pre_queue，後處理    │
                    │       Write Queue；同時仍更新 Circular Buffer│
                    └──────┬───────────────────────┬──────────────┘
                           │ TRIGGER_STOP           │ 排程結束 or
                           ▼                        │ 手動停止
                    ┌──────────────────┐            │
                    │ REC_STATE_       │            │
                    │ POST_WAIT        │            │
                    │ 動作：           │            │
                    │  重設倒數計時器  │            │
                    │  (post_sec)      │            │
                    └──────┬───────┬──┘            │
                           │       │                │
               倒數歸零    │       │ TRIGGER_START  │
                           ▼       └──────────────→ IN_EVENT
                        IDLE
```

### 7.2 EXTRACT_PRE 對齊演算法

**設計原則：**
Event loop thread **只做 index 掃描與 entry 複製**，不執行 memcpy 幀資料。
實際的 byte 複製由 Writer thread 消費 pre_extract_queue 時從 ring 讀出。
Ring buf 以 `protected_read_offset / protected_read_size` 保護預錄區間不被 wrap-around 覆蓋。

```
─── Event loop thread（EXTRACT_PRE 進入時執行，< 0.1ms）──────────

輸入：
  pre_sec  — 目標預錄秒數（e.g. 10s）
  now_ns   — 當前時間（CLOCK_MONOTONIC）

1. 計算目標時間
   target_ns = now_ns - pre_sec * 1_000_000_000

2. 掃描 Index Table，找起始 keyframe
   start_idx = INVALID
   for i in [index_head .. index_tail):
       if index[i].timestamp_ns >= target_ns and index[i].is_keyframe:
           start_idx = i
           break
   if start_idx == INVALID:
       /* 嘗試回退到最老的 keyframe */
       for i in [index_head .. index_tail):
           if index[i].is_keyframe:
               start_idx = i
               break
   if start_idx == INVALID:
       /* 完全沒有 keyframe：切換至 WAIT_KEYFRAME，等首個 IDR 到來才開始寫入 */
       /* 不設保護區間（無預錄），live frame 待 IDR 後才送入 Write Queue */
       state = REC_STATE_WAIT_KEYFRAME
       return  /* 跳過後續步驟 3~7 */

3. 計算保護區間（Issue #1 修正：正確處理 ring 環繞）
   protect_start = index[start_idx].offset

   /* 計算預錄段的總 byte 長度，需考慮 write_pos 可能已環繞 */
   if write_pos >= protect_start:
       protect_size = write_pos - protect_start
   else:
       protect_size = ring_size - protect_start + write_pos
   /* protect_size = 0 代表預錄段為空，不需保護，直接跳過 */

4. 設定 ring 保護（release order：先寫 size，再寫 offset）
   atomic_store_explicit(&ring->protected_read_size,   protect_size,   memory_order_release)
   atomic_store_explicit(&ring->protected_read_offset, protect_start,  memory_order_release)
   /* Writer 以 acquire order 讀取，確保可見性 */

5. 將 [start_idx .. index_tail) 的 index entry 依序推入 pre_extract_queue
   for i in [start_idx .. index_tail):
       pre_queue_enqueue(&ring->pre_queue, &index[i])   /* 複製 ~32 bytes entry，無幀資料 memcpy */
   /* 若 pre_queue 滿（理論上不會發生，深度 = REC_FRAME_INDEX_MAX），log ERROR 並截斷 */

6. 通知 Writer thread（透過 pre_queue 非空即可，Writer 靠 eventfd 或輪詢感知）

7. 立即轉移至 IN_EVENT（event loop 不等待 Writer 完成）

─── Writer thread（統一輸出路徑）──────────────────────────────
/*
 * Writer 有兩個輸入來源，優先序固定：
 *   (A) pre_extract_queue — 必須先 drain 完
 *   (B) rec_write_queue   — live frames，在 pre_queue drain 完後才處理
 *
 * 核心原則：Writer 直接將資料送至 TS muxer（rec_segment_write），
 *            ★ 不將 pre_queue 資料 re-enqueue 至 Write Queue ★
 *            這樣才能保證時序：pre-roll 幀永遠早於 live 幀寫入檔案。
 */

/* Phase A：drain pre_extract_queue */
while pre_queue 非空:
    entry = pre_queue_dequeue(&ring->pre_queue)

    /* memcpy 在 Writer thread 執行，不阻塞 event loop */
    tmp_buf = malloc(entry.size)
    if entry.offset + entry.size <= ring_size:
        memcpy(tmp_buf, ring->ring + entry.offset, entry.size)
    else:
        /* 跨越 ring 尾端，分兩段複製 */
        first_part = ring_size - entry.offset
        memcpy(tmp_buf,              ring->ring + entry.offset, first_part)
        memcpy(tmp_buf + first_part, ring->ring,                entry.size - first_part)

    /* 直接送 TS muxer，不走 Write Queue */
    rec_segment_write(seg_ctx, tmp_buf, entry.size, entry.timestamp_ns, entry.is_keyframe)
    free(tmp_buf)

    /* 清除 ring 保護的時機：最後一個 entry 的 memcpy「完成後」，不是 dequeue 後 */
    if pre_queue 已空:
        atomic_store_explicit(&ring->protected_read_offset, REC_PROTECT_NONE, memory_order_release)
        atomic_store_explicit(&ring->protected_read_size,   0,                memory_order_release)

/* Phase B：處理 live Write Queue（pre_queue drain 後才到達此處）*/
/* 正常 Writer loop，從 rec_write_queue dequeue → rec_segment_write → free */

─── rec_buf_push() wrap-around 保護（event loop）──────────────

/* 寫入新幀前，若新幀 byte range 與保護區間重疊，自旋等待 */
/* overlap 判斷需處理環繞情境，見下方 rec_buf_overlaps() */
uint32_t protect_off  = atomic_load_explicit(&ring->protected_read_offset, memory_order_acquire)
uint32_t protect_size = atomic_load_explicit(&ring->protected_read_size,   memory_order_acquire)

if protect_off != REC_PROTECT_NONE and protect_size > 0:
    struct timespec spin_start
    clock_gettime(CLOCK_MONOTONIC, &spin_start)
    while rec_buf_overlaps(write_pos, new_size, protect_off, protect_size, ring_size):
        if elapsed_us(spin_start) > REC_PROTECT_SPIN_TIMEOUT_US:
            /* Writer 卡住或死亡：abort 本次 event clip，清除保護旗標，回 IDLE */
            log_error("%s:%d protect spin timeout (%d us), aborting event clip",
                      __func__, __LINE__, REC_PROTECT_SPIN_TIMEOUT_US)
            atomic_store_explicit(&ring->protected_read_offset, REC_PROTECT_NONE, memory_order_release)
            atomic_store_explicit(&ring->protected_read_size,   0,                memory_order_release)
            rec_state_force_idle(eng)   /* 通知 State Machine 放棄此次 event */
            return REC_ERR_WRITER_STUCK
        cpu_relax()

/* 繼續寫入（淘汰舊 index entry，memcpy 幀資料至 ring）*/

─── rec_buf_overlaps()（環形 overlap 判斷）────────────────────

/*
 * 判斷 [a_off, a_off+a_size) 與 [b_off, b_off+b_size) 在環形空間中是否重疊。
 * 環形大小為 ring_size。兩個區間均不超過 ring_size。
 */
static bool rec_buf_overlaps(uint32_t a_off, uint32_t a_size,
                              uint32_t b_off, uint32_t b_size,
                              uint32_t ring_size)
{
    /* 將環形區間展開為線性區間比較（最多一次折疊）*/
    uint32_t a_end = a_off + a_size;   /* 可能 > ring_size（代表環繞）*/
    uint32_t b_end = b_off + b_size;

    if (a_end <= ring_size and b_end <= ring_size):
        /* 兩者均未環繞，直接比較 */
        return (a_off < b_end) and (b_off < a_end)
    else:
        /* 至少一個環繞，分段展開：展開成 [0, ring_size*2) 空間比較 */
        /* 簡化實作：將 b 區間展開，分兩段判斷 a 是否與任一段重疊 */
        bool overlap1 = rec_buf_linear_overlap(a_off, a_size,
                                               b_off, min(b_size, ring_size - b_off))
        bool overlap2 = (b_end > ring_size) and
                        rec_buf_linear_overlap(a_off, a_size,
                                               0, b_end - ring_size)
        return overlap1 or overlap2
}
```

---

## 八、排程引擎設計

### 8.1 時間位元遮罩

一天 96 個格（15 分鐘/格），每格 2 bits（off / continuous / event）：

```c
/* 192 bits = 24 bytes 存一天的排程 */
typedef struct {
    uint8_t slots[24];   /* bits[i*2 .. i*2+1] = rec_slot_mode_t for slot i */
} rec_schedule_day_t;

/* 週排程 */
typedef struct {
    rec_schedule_day_t days[7];   /* days[0] = 週日，days[1] = 週一，... */
} rec_schedule_t;
```

### 8.2 查詢當前應執行的模式

```c
rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched,
                                   time_t now_wall_clock)
{
    struct tm t;
    localtime_r(&now_wall_clock, &t);
    int slot = (t.tm_hour * 60 + t.tm_min) / 15;   /* 0 ~ 95 */
    int bit_offset = slot * 2;
    uint8_t byte = sched->days[t.tm_wday].slots[bit_offset / 8];
    return (rec_slot_mode_t)((byte >> (bit_offset % 8)) & 0x3);
}
```

### 8.3 計時器驅動

排程邊界切換與 POST_WAIT 倒數共用同一個 **1 秒週期 `timerfd`**：

```c
/* rec_engine_create() 內部初始化 */
int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
struct itimerspec its = {
    .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
    .it_value    = { .tv_sec = 1, .tv_nsec = 0 },
};
timerfd_settime(timer_fd, 0, &its, NULL);
/* timer_fd 加入 rec_engine_get_epoll_fds() 回傳列表 */
```

每次 timerfd 觸發（EPOLLIN）時，`rec_engine_handle_event()` 執行：

```c
/* 1. 正確消耗 timerfd expiration count（必要，否則 epoll 持續觸發）*/
uint64_t expirations;
read(timer_fd, &expirations, sizeof(expirations));   /* expirations 通常為 1，event loop stall 時可 > 1 */

/* 2. POST_WAIT 倒數 */
if (state == REC_STATE_POST_WAIT) {
    post_remaining_sec -= (int)expirations;          /* stall 補償，避免計時漂移 */
    if (post_remaining_sec <= 0 && !pending_trigger)
        rec_state_transition(eng, REC_STATE_IDLE);
}

/* 3. 排程邊界檢查（每 tick 都查，rec_schedule_query 為純位元運算，< 1 µs）*/
time_t now = time(NULL);
rec_slot_mode_t slot = rec_schedule_query(&eng->config.schedule, now);
if (slot != eng->current_slot) {
    eng->current_slot = slot;
    rec_apply_schedule_change(eng, slot);
}
```

**為什麼不靠幀間隔計時？**
低 fps（1 fps）場景下，幀間隔最長 1 秒，計時精度差；且 IDLE 狀態可能長時間無幀。
timerfd 精度為 CLOCK_MONOTONIC，不受 NTP 跳秒影響。

**為什麼每 tick 都查排程，不是每 60 tick？**
event loop stall 時 `expirations > 1`，啟動偏移（程序在格中途啟動）時也需立即偵測；
`rec_schedule_query()` 代價極低，無需節流。

---

## 九、異步寫入引擎

### 9.1 設計原則

- Write Queue 為 bounded SPSC queue（Single Producer Single Consumer）
  - **唯一 Producer = event loop thread**（live frame path，IN_EVENT 狀態）
  - Consumer = Writer thread（獨立執行緒，唯一允許的例外）
  - ★ Writer 不得向 Write Queue enqueue 任何資料（包括 pre-roll 幀）★
- Writer thread 有兩個輸入來源，**優先序固定**：
  1. `pre_extract_queue`（pre-roll path）— 必須完全 drain 後才處理下一來源
  2. `rec_write_queue`（live path）— pre_queue 排空後方可處理
  - 此優先序確保 pre-roll 幀永遠寫在 live 幀之前，時序不亂
- Writer thread 負責：封裝為 MPEG-TS → `write()` → 分段判斷
- 主 event loop 與 Writer thread 唯一共享的資料結構是 Write Queue 與 rec_buf_t 的 pre_queue / protect 欄位

### 9.2 Write Queue 結構

```c
#define REC_WRITE_QUEUE_DEPTH  256    /* 最多 256 個待寫幀 */

typedef struct {
    rec_write_item_t  items[REC_WRITE_QUEUE_DEPTH];
    _Atomic uint32_t  head;    /* Writer 讀取位置 */
    _Atomic uint32_t  tail;    /* Producer 寫入位置 */
} rec_write_queue_t;
```

SPSC queue 使用兩個 atomic index，無 mutex，Writer thread 用 `eventfd` 喚醒。

### 9.3 MPEG-TS 封裝

```
每個 encoded frame → 切割為 188-byte TS packets
Header：sync byte(0x47) + PID + continuity counter + adaptation field（PCR）
Payload：PES header（含 PTS）+ NAL data

新 segment / event clip 起點輸出順序（必須完整，播放器才能獨立解）：
  1. PAT（Program Association Table）
  2. PMT（Program Map Table）
  3. Codec Config NALUs（來自 rec_codec_config_t cache）：
       H.264：SPS + PPS
       H.265：VPS + SPS + PPS
  4. IDR slice（第一個幀資料）

若 encoder 本身在每個 IDR 重送 config NALUs（IAV 預設行為），
Writer 偵測到 config NALUs 後更新 cache 並直接透傳，無需重複插入；
若 encoder 不重送，Writer 主動從 cache 插入（cache 為空時推遲開檔，直到收到首個帶 config 的 IDR）。
```

自行實作基本 TS muxer（約 300 行），不引入 FFmpeg 等大型依賴。

### 9.4 檔案命名規則

```
<output_dir>/<stream_name>_<YYYYMMDD>_<HHMMSS>_<mode>.ts
例：
  cam0_20260424_143000_cont.ts      ← 連續錄影
  cam0_20260424_150230_event.ts     ← 事件錄影
```

### 9.5 fdatasync 策略

```
Recorder 在以下時機呼叫 fdatasync(fd)：
  1. 每個 segment 關閉前（close 前必須 sync，保證 segment 完整落盤）
  2. event clip 結束前（POST_WAIT → IDLE 轉移，sync 後才關檔）
  3. 可選週期性 flush：config 參數 flush_interval_sec（預設 60s，0 = 停用）
     → Writer 每 N 秒呼叫 fdatasync，確保持續錄影模式下 crash recovery 窗口有界

斷電可恢復語意（明確聲明）：
  - 可播放至最近一次 fdatasync 完成的資料
  - page cache 中尚未 sync 的資料（最多 flush_interval_sec 的量）可能丟失
  - fdatasync 代價：4K SSD ~1ms，eMMC ~5ms；每 10 分鐘 segment 結束時的 sync 屬正常範圍
```

新增 config 欄位（見 §十 `rec_config_t`）：
```c
uint32_t  flush_interval_sec;   /* 週期性 fdatasync，0 = 僅 segment/clip 結束時 sync */
```

### 9.6 Write Queue 溢出：drop-until-IDR 策略

任意丟幀會讓解碼器在非 IDR 邊界重試，造成花屏直到下一個 IDR。正確策略：

```
溢出時（Write Queue 已滿，新幀無法 enqueue）：
  1. 設 eng->overflow_drop = true，drop_count++
  2. 繼續丟棄後續每幀（++drop_count），直到 is_keyframe == true

收到 IDR（is_keyframe == true）且 overflow_drop == true：
  1. 重置 overflow_drop = false
  2. 將此 IDR 的 is_segment_boundary 設 true（Writer 切新檔，帶 PAT+PMT+config）
  3. 正常 enqueue 此 IDR

效果：解碼器始終從完整 GOP 邊界重啟，不花屏。
告警：overflow_drop 期間應同時遞增 drop_count 並觸發監控告警（磁碟 I/O 嚴重過載）。
```

---

## 十、對外 API（`rec_engine.h`）

```c
typedef struct rec_engine rec_engine_t;

/* ─── 設定參數 ───────────────────────────────────────────────────── */
typedef struct {
    char              stream_name[VFR_SOCKET_NAME_MAX];
    rec_mode_t        default_mode;
    rec_schedule_t    schedule;
    uint32_t          pre_record_sec;
    uint32_t          post_record_sec;
    uint32_t          segment_duration_sec;
    uint64_t          segment_size_max;   /* bytes，0 = 只用時間切 */
    uint32_t          ring_buf_size;
    uint32_t          flush_interval_sec;  /* 週期性 fdatasync，0 = 僅 segment/clip 結束時 */
    char              output_dir[256];
} rec_config_t;

/*
 * rec_engine_create()：
 *   建立錄影引擎，初始化所有子模組。
 *   內部呼叫 vfr_open() 取得 VFR consumer context。
 * 回傳：成功回傳 handle；失敗回傳 NULL
 */
rec_engine_t *rec_engine_create(const rec_config_t *cfg);

/*
 * rec_engine_get_epoll_fds()：
 *   回傳需加入呼叫者 epoll 的 fd 列表。
 *   目前回傳的 fd（依序，共 3 個）：
 *     [0] VFR consumer eventfd    — 新幀到達通知
 *     [1] AI trigger socket fd    — 跨 process 觸發訊號（Unix abstract socket）
 *     [2] timerfd（1 秒週期）     — POST_WAIT 倒數 + 排程邊界切換
 *   max_fds 建議傳入 REC_MAX_EPOLL_FDS。
 *   回傳值：實際填入的 fd 數量；< 0 表示錯誤。
 *
 *   ★ epoll 所有權：由呼叫者全權管理 ★
 *   呼叫者負責 EPOLL_CTL_ADD（取得 fd 後）與 EPOLL_CTL_DEL（destroy 前）。
 *   rec_engine_destroy() 不操作 epoll；呼叫者必須在呼叫 destroy() 之前，
 *   先對所有已取得的 fd 執行 EPOLL_CTL_DEL，否則 epoll 可能持有 dangling fd。
 */
int rec_engine_get_epoll_fds(rec_engine_t *eng, int *fds_out, int max_fds);

/*
 * rec_engine_handle_event()：
 *   在 epoll 返回後呼叫，處理對應 fd 上的事件。
 *   設計為非阻塞（不得在此函數內呼叫 sleep / 長時間等待 / 大型 memcpy）。
 * 回傳：0 = 正常；-1 = 不可恢復錯誤
 */
int rec_engine_handle_event(rec_engine_t *eng, int fd);

/*
 * rec_engine_destroy()：
 *   停止錄影、排空 Write Queue、關閉所有資源。
 *   *eng 設為 NULL；重複呼叫為 no-op。
 */
void rec_engine_destroy(rec_engine_t **eng);

/* ─── 監控介面 ───────────────────────────────────────────────────── */
rec_state_t rec_engine_get_state(const rec_engine_t *eng);
uint64_t    rec_engine_get_written_bytes(const rec_engine_t *eng);
uint32_t    rec_engine_get_dropped_frames(const rec_engine_t *eng);
```

### 10.1 `rec_engine_destroy()` 資源釋放順序（Issue #4 修正）

```
前置條件（呼叫者責任）：
  呼叫者必須在進入 rec_engine_destroy() 前，對 rec_engine_get_epoll_fds() 回傳的
  所有 fd 執行 EPOLL_CTL_DEL，否則 epoll 持有 dangling fd。

1.  enqueue sentinel item（is_shutdown_sentinel = true，data = NULL）至 Write Queue
      → Writer thread 收到後排空 queue、逐一 free(data)、自行結束

2.  pthread_join(writer_thread)
      → 確保 Writer 完全結束，ring buf 不再有人讀寫

3.  close(timer_fd)           /* 呼叫者已 EPOLL_CTL_DEL，可直接 close */
4.  close(trigger_socket_fd)

5.  vfr_close(&vfr_ctx)       /* Writer 已 join，ring buf 無競爭，可安全釋放 */

6.  free(ring->ring)
7.  free(rec_buf)             /* pre_queue 屬於 rec_buf_t，一同釋放 */
8.  free(rec_engine)
9.  *eng = NULL
```

**重複呼叫防護**：每個資源關閉後設為 -1 / NULL，第二次呼叫 `rec_engine_destroy` 為 no-op。

---

## 十一、開發階段（MVP Roadmap）

### Phase R1：Circular Buffer + I-Frame Indexer

**目標**：底層幀緩衝正確性驗證，含 protect 機制

實作項目：
- `rec_buf.c`：byte-ring 寫入、wrap-around 淘汰、`rec_buf_overlaps()`
- `rec_buf.h`：`rec_buf_push()`、`rec_buf_extract_from_keyframe()`、`rec_pre_queue_t`
- `test/test_rec_buf.c`

驗收 Checklist：
- [ ] 單元測試：寫入 1000 幀，buffer 滿後舊幀自動淘汰，index_head 正確推進
- [ ] I-Frame 定位：`extract_from_keyframe(t_now - 10s)` 回傳正確起始 entry
- [ ] Wrap-around：buffer 環繞後 index offset 計算正確，無越界
- [ ] **保護區間正確性（Issue #1 修正）**：
  - 模擬 Writer 持有 protect 區間期間，`rec_buf_push()` 自旋（不覆蓋）
  - 模擬環繞情境下的 protect_size 計算：`write_pos < protect_start` 時結果正確
  - 保護清除後，`rec_buf_push()` 立即恢復正常寫入
  - 自旋超過 `REC_PROTECT_SPIN_TIMEOUT_US` 時，強制 break 並輸出 ERROR log
- [ ] `rec_buf_overlaps()` 單元測試：非環繞、單邊環繞、雙邊環繞各場景
- [ ] valgrind / ASan：無記憶體洩漏、無越界讀寫

---

### Phase R2：狀態機（無寫入，僅 Log）

**目標**：驗證狀態轉移邏輯，不依賴 I/O

實作項目：
- `rec_state.c`：四狀態轉移 + 消抖器 + POST_WAIT 倒數
- `rec_debounce.c`：時間過濾（`REC_DEBOUNCE_MS`）
- `rec_trigger.c`：Unix socket listener（接 `vfr_event_msg_t`）
- `test/test_rec_state.c`：mock trigger 注入，驗證狀態序列

驗收 Checklist：
- [ ] IDLE → EXTRACT_PRE → IN_EVENT：觸發後狀態正確轉移
- [ ] POST_WAIT → IN_EVENT：延時期間收到新觸發，正確回歸
- [ ] POST_WAIT → IDLE：倒數 `post_record_sec` 秒後無新觸發，回到待命
- [ ] 消抖器：500ms 內重複 START 不觸發第二次轉移
- [ ] 排程關閉時段內，TRIGGER_START 被靜默忽略
- [ ] timerfd 驅動 POST_WAIT 倒數（不靠幀間隔），驗證 IDLE 狀態下倒數仍正確觸發

---

### Phase R3：異步寫入 + 分段

**目標**：實際產生可播放的 .ts 檔案

實作項目：
- `rec_writer.c`：Write Queue + pre_extract_queue 消費 + Writer thread + MPEG-TS 封裝
- `rec_segment.c`：按時間 / 大小切檔、`is_segment_boundary` 旗標設定
- `test/test_rec_writer.c`：模擬 encoded frames 餵入，驗證輸出檔案

驗收 Checklist：
- [ ] 產生的 .ts 可用 `ffprobe` 正確讀取 duration、codec、PTS
- [ ] 分段切換：在 I-Frame 邊界切換，新 segment 從 PAT+PMT+IDR 開始
- [ ] `is_segment_boundary` 由 `rec_segment.c` 在 enqueue 前正確設定
- [ ] pre_extract_queue 消費：Writer 正確從 ring 讀出預錄段，並在最後一個 entry memcpy 完成後清除 protect
- [ ] Write Queue 溢出：Producer 速度 > Writer 速度時，`drop_count` 遞增，不 crash
- [ ] destroy 流程：sentinel item 送入後 Writer 正常排空並結束，`pthread_join` 不 hang
- [ ] 斷電模擬：截斷最後一個 .ts，仍可播放前面的 packets（TS 特性）
- [ ] ASan：無記憶體錯誤

---

### Phase R4：排程 + 全流程整合

**目標**：接上真實 VFR，在 Amba 平台驗證完整流程

實作項目：
- `rec_schedule.c`：時間位元遮罩查詢
- `rec_engine.c`：整合所有子模組，提供 `rec_engine_create/handle/destroy`
- `platform/amba/amba_adapter.c`：IAV bitstream 讀取（`VFR_FLAG_KEY_FRAME`）
- `test/test_rec_full.c`：端到端測試

驗收 Checklist：
- [ ] 24/7 模式：持續錄影 1 小時，每 10 分鐘自動分段；**前提：磁碟寫入速度足夠（斷言 `drop_count == 0`）**
- [ ] 事件模式：AI trigger → 預錄對齊 I-Frame → 錄影 → POST_WAIT → 結束
- [ ] 排程模式：時段 off → 無輸出；時段 continuous → 正常錄影
- [ ] 跨 process 觸發：AI process 送 `vfr_event_msg_t`，Recorder 正確響應
- [ ] VFR Watchdog 整合：Recorder crash 後，VFR server 正確回收 slot
- [ ] `rec_engine_destroy()` 順序驗證：`valgrind` 無 leak，`/proc/PID/fd` 計數恢復
- [ ] Prometheus metrics：`rec_drop_frames`、`rec_written_bytes`、`rec_state` 可 scrape

---

## 十二、嚴格禁止事項

| 禁止行為 | 原因 |
|----------|------|
| 在 event loop thread 呼叫 `write()` | 磁碟 I/O 阻塞，所有 VFR 幀事件卡死 |
| 在 event loop thread 呼叫 `sleep()` / `usleep()` | 同上 |
| 在 event loop thread 對大型 buffer 執行**批次/多幀連續 `memcpy()`** | ARM DDR 競爭下，60MB pre-roll memcpy 可達 30ms+；EXTRACT_PRE 只做 index entry enqueue，pre-roll memcpy 統一由 Writer thread 執行。單幀 live copy（≤ 500 KB / ~250 µs）在 event loop 執行是可接受的 |
| Writer thread 直接讀取 ring buf 不設 protected_read_offset | Data race；ring 可能被 wrap-around 覆蓋；必須先設保護再讀 |
| Writer 在 dequeue 後立即清除 protected_read_offset | memcpy 尚未完成，event loop 可能提前覆蓋保護區間；清除時機必須是 memcpy 完成後 |
| 使用 `write_pos - protect_start` 計算 protect_size，不處理環繞 | uint32 underflow，保護區間計算錯誤，overlap 判斷失效 |
| rec_buf_push() 自旋超過 REC_PROTECT_SPIN_TIMEOUT_US 仍繼續等待 | Writer 可能已死，無限自旋；逾時後強制 break 並 log ERROR |
| Write Queue 溢出時 block event loop | 應 drop frame 並遞增 `drop_count`；VFR BLOCK_PRODUCER 與 Write Queue drop 是兩個不同層級 |
| 錄影段從非 I-Frame 開始 | 解碼器輸出綠屏或崩潰 |
| 使用 `signal()` 設定 handler | 多執行緒行為未定義；改用 `sigaction()` |
| EXTRACT_PRE 期間持有 VFR slot | `VFR_MAX_CONSUMER_SLOTS=4`，預錄需數百幀，必須在 event loop memcpy 後立即 `vfr_put_frame()` |
| 直接存取 `vfr_server_t` 或 `vfr_pool_t` 內部欄位 | 違反 VFR opaque 設計；改用公開 API |
| Write Queue 使用 mutex（主路徑）| SPSC queue 用 atomic index；mutex 僅用於 flush / destroy |
| `free(item->data)` 在 event loop 端執行 | data 所有權屬 Writer thread；destroy 必須 join Writer 後才能釋放 engine |
| 直接讀寫 `vfr_frame_t.stride` 當 stream_id | 語意不明；改用 `REC_FRAME_STREAM_ID()` / `REC_FRAME_SET_STREAM_ID()` |
| POST_WAIT / 排程切換靠幀間隔計時 | 低 fps 或 IDLE 時幀間隔長，精度差；應靠 1 秒週期 timerfd |
| destroy 時未 join Writer 就 free ring buf | Writer 可能仍在 memcpy，use-after-free |
| Writer 將 pre_queue 資料 re-enqueue 至 Write Queue | 打破 SPSC 單一生產者；pre-roll 幀與 live 幀時序混亂；Writer 直接輸出至 TS muxer |
| Write Queue 溢出時任意丟幀 | 解碼器在非 IDR 邊界重試造成花屏；應 drop-until-next-IDR 並標記 is_segment_boundary（見 §9.6）|
| 保護自旋逾時後繼續覆蓋保護區間 | Writer 已異常，繼續寫入會破壞尚未讀出的 pre-roll 資料；應 abort event clip 並回 IDLE |
| 未讀取 timerfd expiration count 直接忽略 | event loop stall 後 epoll 持續觸發；timerfd 必須 read 8 bytes 消耗 uint64 expiration count |
| destroy 前未對所有 rec_engine_get_epoll_fds() 回傳的 fd 執行 EPOLL_CTL_DEL | engine close fd 後 epoll 持有 dangling fd；所有 EPOLL_CTL_DEL 是呼叫者責任，destroy 不操作 epoll |
| segment 起點不輸出 SPS/PPS（H.264）或 VPS/SPS/PPS（H.265） | 大多播放器無法獨立解碼；PAT+PMT 不夠，codec config NALUs 必須在 IDR 前輸出 |
| 宣稱「斷電資料完全可恢復」而不說明 fdatasync 節奏 | page cache 中的資料掉電即丟；應明確聲明「可恢復到最近一次 fdatasync 前」|

---

## 十三、關鍵設計決策表

| 項目 | 決策 | 原因 |
|------|------|------|
| 容器格式 | MPEG-TS | 斷電可恢復；無需 moov atom；TS 相容性廣 |
| Circular Buffer 型態 | Byte-ring + Frame Index Table | Encoded frame 變長，不能用固定 slot |
| 預錄記憶體所有權 | Recorder 自有（Writer thread memcpy 自 ring）| `VFR_MAX_CONSUMER_SLOTS=4` 不夠存百幀；memcpy 在 Writer thread 執行，不阻塞 event loop |
| 寫入執行緒數量 | 1 個 Writer thread | SPSC queue 無 mutex，更簡單可靠 |
| pre_extract_queue 位置 | 屬於 `rec_buf_t`，SPSC atomic queue | 與 ring 的 protect 機制同結構，便於一致管理 |
| protect 清除時機 | 最後一個 entry 的 memcpy 完成後 | dequeue 後即清除會導致 event loop 在 memcpy 期間覆蓋保護區間 |
| protect_size 計算 | 環繞修正公式（see §七） | `write_pos - protect_start` 在環繞時 underflow，導致 overlap 判斷失效 |
| 自旋逾時 | `REC_PROTECT_SPIN_TIMEOUT_US = 2000 µs` | 15MB / ~8 GB/s DDR ≈ 1.9ms 最差情況；2ms 夠用，超過代表 Writer 異常 |
| AI 觸發通道 | Unix abstract socket | 跨 process；延續 VFR IPC 慣例 |
| I-Frame 旗標 | `VFR_FLAG_KEY_FRAME` in `vfr_frame_t.flags` | 最小化 VFR 改動，platform adapter 填入 |
| IAV bitstream 取得 | memfd（初期）→ dma-buf（優化）| 先求正確，再求零複製 |
| MPEG-TS 實作 | 自實作基本 TS muxer | 避免引入 FFmpeg 的大型依賴 |
| 排程粒度 | 15 分鐘 | 2 bits × 96 slots = 24 bytes/天，足夠實用 |
| Write Queue 深度 | 256 | 256 × ~40KB avg = ~10MB，可吸收 I/O 抖動 |
| destroy 順序 | sentinel → join → close fds → vfr_close → free | 確保 Writer 不 use-after-free ring buf |
| epoll 所有權 | 完全由 caller 管理（EPOLL_CTL_ADD/DEL 均由 caller 負責） | engine 無 epoll fd；caller destroy 前先 DEL，避免 dangling fd |
| Writer 輸出路徑 | pre_queue → TS muxer 直接；Write Queue → TS muxer；兩路皆不 re-enqueue | 維持 SPSC 不變（唯一 producer = event loop），保證 pre-roll 在 live 之前寫入 |
| Write Queue 溢出丟幀策略 | drop-until-next-IDR + is_segment_boundary | 避免解碼器在 non-IDR 邊界花屏；丟棄單位是完整 GOP |
| fdatasync 節奏 | segment close + event clip end + 可選週期（default 60s） | 限定掉電丟失窗口；定義「斷電可恢復」語意的可測量邊界 |
| 無 keyframe fallback | 進 WAIT_KEYFRAME 狀態，等首個 IDR 才開檔 | 不允許 clip 從 P-frame 開始；WAIT_KEYFRAME 是顯式狀態，不是隱式 flag |
| codec config 快取 | rec_codec_config_t；每個 segment/event 起點由 Writer 主動插入 | 確保每個 clip 可獨立解碼；不依賴 encoder 重送行為 |
| 自旋逾時後的行為 | abort event clip + 清除保護 + 回 IDLE | 逾時代表 Writer 異常，繼續覆蓋會破壞 pre-roll；fail-safe 行為 |

---

## 十四、相依套件

| 套件 | 用途 | 最低版本 | 必要性 |
|------|------|----------|--------|
| Linux kernel | pidfd, eventfd, memfd, timerfd, futex | 5.4+ | 必要 |
| VFR（本專案） | 幀來源 | Phase 5 完成版 | 必要 |
| Ambarella IAV | HW encode bitstream | IAV5 / IAV6 | Amba 平台必要 |
| pthreads | Writer thread | glibc | 必要 |
| *(自實作 TS muxer)* | MPEG-TS 封裝 | — | 無外部依賴 |

---

## 十五、Build 系統

```makefile
# Recorder 模組（純 C，不依賴外部函式庫）
SRCS_REC = \
    rec/rec_buf.c \
    rec/rec_state.c \
    rec/rec_schedule.c \
    rec/rec_trigger.c \
    rec/rec_debounce.c \
    rec/rec_writer.c \
    rec/rec_segment.c \
    rec/rec_engine.c

# Phase R1
test_rec_buf: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) \
              rec/rec_buf.c test/test_rec_buf.c
	$(CC) $(CFLAGS) $(INCLUDES) -Irec -o $@ $^

# Phase R4（全流程，需連結 pthread）
test_rec_full: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) \
               $(SRCS_IPC_SERVER) $(SRCS_REC) test/test_rec_full.c
	$(CC) $(CFLAGS) $(INCLUDES) -Irec -o $@ $^ -lpthread

checkR1: test_rec_buf
	./test_rec_buf

checkR4: test_rec_full
	./test_rec_full --self-test
```

---

*文件版本：v1.3 — 2026-04-24*
*v1.3 修正：9 個設計問題*
*  🔴 thread model 自相矛盾（Writer re-enqueue → 直接輸出 TS）*
*  🔴 live frame copy 與禁止事項衝突（精確化：禁批次 memcpy，單幀可接受）*
*  🔴 保護自旋逾時過短 + 逾時後行為錯誤（2ms → 100ms，abort event）*
*  🟡 SPS/PPS/VPS 未在 segment 起點保證輸出（新增 codec config cache）*
*  🟡 epoll 所有權不一致（明確：caller 全權管理）*
*  🟡 斷電可恢復過度承諾（新增 §9.5 fdatasync 策略）*
*  🟢 排程 60-tick 延遲（改為每 tick 查 wall clock + 正確 drain expiration count）*
*  🟢 no-keyframe fallback 不清（新增 WAIT_KEYFRAME 狀態）*
*  🟢 Write Queue overflow 任意丟幀（改為 drop-until-IDR + is_segment_boundary）*
*v1.2 修正：6 個設計問題（1 🔴 / 3 🟡 / 2 🟢）*
*v1.1 修正：10 個設計問題（3 🔴 / 4 🟡 / 3 🟢）*
*基於 plan-vfr-cross-platform-v2.md v2.4 設計，VFR 為幀來源層，Recorder 為 VFR 消費者模組*
