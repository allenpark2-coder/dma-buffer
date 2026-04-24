# VFR Recording Engine — 設計計畫書
# plan-recorder-v1.md

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
- **斷電可恢復**：採 MPEG-TS 容器，逐 packet 寫入

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
│   ├── rec_buf.c / rec_buf.h     # Circular Byte Ring + Frame Index Table
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
/* 目前已知 fd 來源：VFR eventfd(1) + AI trigger socket(1) + timerfd(1) */
#define REC_MAX_EPOLL_FDS  8             /* 保留 2x 彈性，caller buffer 至少此大小 */

/* ─── stream_id accessor（stride 欄位語意重用）────────────────────── */
/*
 * vfr_frame_t.stride 在 encoded 模式下存放 stream_id。
 * 使用 macro 存取，避免 implicit reuse 造成混淆。
 * 未來如 VFR 新增 priv 欄位，改此 macro 即可，呼叫端不動。
 */
#define REC_FRAME_STREAM_ID(frame)        ((uint32_t)(frame)->stride)
#define REC_FRAME_SET_STREAM_ID(frame, id) ((frame)->stride = (uint32_t)(id))

/* ─── 錄影模式 ───────────────────────────────────────────────────── */
typedef enum {
    REC_MODE_CONTINUOUS = 0,  /* 24/7 連續錄影 */
    REC_MODE_SCHEDULED  = 1,  /* 依排程位元遮罩 */
    REC_MODE_EVENT      = 2,  /* 純事件錄影（無觸發則不錄）*/
} rec_mode_t;

/* ─── 狀態機狀態 ─────────────────────────────────────────────────── */
typedef enum {
    REC_STATE_IDLE         = 0,  /* 待命：僅執行環形緩衝 */
    REC_STATE_EXTRACT_PRE  = 1,  /* 預錄提取：將緩衝區送入 Write Queue */
    REC_STATE_IN_EVENT     = 2,  /* 錄影中：即時幀導向 Write Queue */
    REC_STATE_POST_WAIT    = 3,  /* 延時等待：倒數，可被新觸發打斷 */
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

### 4.2 Circular Buffer 幀索引項

```c
/* rec_buf.h */
typedef struct {
    uint32_t offset;        /* 在 byte ring 中的起始偏移 */
    uint32_t size;          /* 此幀 encoded 大小（bytes）*/
    uint64_t timestamp_ns;
    uint64_t seq_num;
    bool     is_keyframe;
} rec_frame_entry_t;

typedef struct rec_buf {
    uint8_t           *ring;          /* 環形 byte 緩衝區（malloc）*/
    uint32_t           ring_size;     /* 總容量（bytes）*/
    uint32_t           write_pos;     /* 下一個寫入位置 */

    rec_frame_entry_t  index[REC_FRAME_INDEX_MAX];
    uint32_t           index_head;   /* 最老的幀（FIFO head）*/
    uint32_t           index_tail;   /* 下一個寫入位置 */
    uint32_t           index_count;  /* 目前幀數 */

    /*
     * Writer thread 正在讀取的 ring 起始偏移（Issue #1）。
     * Event loop 在 wrap-around 前必須確認新幀的 byte range
     * 不與 [protected_read_offset, protected_read_offset + protected_read_size) 重疊；
     * 若有重疊，需等 Writer 完成當次讀取後才能覆蓋。
     * Writer 讀取前設定此欄位，讀取完畢後清為 REC_PROTECT_NONE。
     *
     * 型別為 _Atomic uint64_t，低 32 bit = offset，高 32 bit = size（或用兩個 atomic）。
     * 此欄位僅在 EXTRACT_PRE 至 Writer 完成預錄段讀取期間有效。
     */
    _Atomic uint32_t   protected_read_offset; /* REC_PROTECT_NONE = UINT32_MAX */
    _Atomic uint32_t   protected_read_size;
} rec_buf_t;

#define REC_PROTECT_NONE  UINT32_MAX

/*
 * rec_buf_push() 內部淘汰規則（Index Table 與 byte ring 同步）：
 *
 * 在寫入新幀前，若新幀的 byte range [write_pos, write_pos + size) 會覆蓋
 * index[index_head] 的 [offset, offset + size) 範圍，
 * 則先推進 index_head（丟棄最老幀的 index entry），重複直到無重疊。
 * 此邏輯確保 index entry 的 offset 永遠指向有效（未被覆蓋）的 ring 資料。
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
} rec_write_item_t;
```

**Write Queue 資料所有權規則（Issue #2）：**

| 動作 | 執行方 | 說明 |
|------|--------|------|
| `malloc(item->data)` | Producer（event loop） | enqueue 前分配，複製幀資料後送入 queue |
| `free(item->data)` | Writer thread | dequeue 後，`write()` 完成即釋放 |
| destroy 時排空 queue | Writer thread 先執行 | Writer 收到 shutdown 訊號後，繼續排空 queue 並逐一 `free(data)`，然後 thread 自行結束 |
| `rec_engine_destroy()` 等待 | Event loop 端 | `pthread_join(writer_thread)` 確保 Writer 完全結束後，才釋放 `rec_engine_t` 及 `rec_buf_t` |

此規則確保：Write Queue 中任何未寫的 item 不會在 destroy 流程中 leak。

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
                    │     距現在 ≥ N 秒的那個                      │
                    │  2. 將 [I-Frame .. ring tail] 的             │
                    │     index entry（offset+size）送入 pre-queue │
                    │     ★ 不在此做 memcpy，不阻塞 event loop ★  │
                    │  3. 設定 ring protected_read_offset          │
                    │  4. 立即切換至 IN_EVENT                     │
                    └──────────────────┬───────────────────────────┘
                                       │ 提取完成（單次，僅 index 操作 < 0.1ms）
                                       ▼
                    ┌──────────────────────────────────────────────┐
                    │ REC_STATE_IN_EVENT                           │
                    │ 動作：每幀直接送入 Write Queue               │
                    │       同時仍更新 Circular Buffer             │
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

**設計原則（Issue #1 修正）：**  
Event loop thread **只做 index 掃描與指標記錄**，不執行 memcpy。  
實際的 byte 複製由 Writer thread 在讀取 pre-queue 時從 ring buf 讀出。  
Ring buf 以 `protected_read_offset / protected_read_size` 保護預錄區間不被 wrap-around 覆蓋。

```
─── Event loop thread（EXTRACT_PRE 進入時執行，< 0.1ms）──────────

輸入：
  pre_sec  — 目標預錄秒數（e.g. 10s）
  now_ns   — 當前時間（CLOCK_MONOTONIC）

1. 計算目標時間
   target_ns = now_ns - pre_sec * 1_000_000_000

2. 掃描 Index Table，找起始 keyframe
   for i in [index_head .. index_tail):
       if index[i].timestamp_ns >= target_ns and index[i].is_keyframe:
           start_idx = i
           break
   if not found:
       start_idx = 最老的 keyframe index

3. 保護 ring buf 區間，防止 Writer 讀取期間被 wrap-around 覆蓋
   // 計算 [start_idx .. index_tail) 的 byte 起始偏移
   protect_start = index[start_idx].offset
   atomic_store(&ring->protected_read_offset, protect_start)
   atomic_store(&ring->protected_read_size,   ring->write_pos - protect_start)
   // （ring 為環形，需做模運算；若 write_pos < protect_start 代表已環繞）

4. 將 [start_idx .. index_tail) 的 index entry 依序推入 pre_extract_queue
   for i in [start_idx .. index_tail):
       enqueue(pre_extract_queue, index[i])   // 只複製 ~32 bytes entry，無 memcpy 幀資料

5. 立即轉移到 IN_EVENT（event loop 不等待 Writer）

─── Writer thread（從 pre_extract_queue 消費）──────────────────

for each entry in pre_extract_queue:
    memcpy(tmp_buf, ring->ring + entry.offset, entry.size)   // 從 ring 讀取幀資料
    enqueue(Write Queue, tmp_buf, entry.size, ...)            // 送入正式 Write Queue
    // 每讀完一個 entry，檢查是否已讀完全部預錄 entry；
    // 讀完後清除 ring 保護：
    //   atomic_store(&ring->protected_read_offset, REC_PROTECT_NONE)

─── rec_buf_push() wrap-around 保護（event loop）──────────────

// 寫入新幀前，若新幀的 [write_pos, write_pos+size) 與 protected 區間重疊，
// 則自旋等待（預期 Writer 很快讀完，ns 級；若超過 1ms 則記錄 warning）
while overlaps(write_pos, size, protected_read_offset, protected_read_size):
    cpu_relax()
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

### 8.3 計時器驅動（Issue #4 & #5 修正）

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
1. **POST_WAIT 倒數**：`post_remaining_sec--`；若歸零且無新觸發，轉移至 IDLE
2. **排程邊界檢查**：每 60 次 tick（= 1 分鐘）呼叫 `rec_schedule_query()` 一次；  
   若當前 15 分鐘格邊界，立即查詢；若模式改變，通知 State Machine

**為什麼不靠幀間隔計時？**  
低 fps（1 fps）場景下，幀間隔最長 1 秒，計時精度差；且 IDLE 狀態可能長時間無幀。
timerfd 精度為 CLOCK_MONOTONIC，不受 NTP 跳秒影響。

---

## 九、異步寫入引擎

### 9.1 設計原則

- Write Queue 為 bounded SPSC queue（Single Producer Single Consumer）
  - Producer = event loop thread（State Machine 送幀）
  - Consumer = Writer thread（獨立執行緒，唯一允許的例外）
- Writer thread 負責：封裝為 MPEG-TS → `write()` → 分段判斷
- 主 event loop 與 Writer thread 唯一共享的資料結構是 Write Queue

### 9.2 Write Queue 結構

```c
#define REC_WRITE_QUEUE_DEPTH  256    /* 最多 256 個待寫幀 */

typedef struct {
    rec_write_item_t  items[REC_WRITE_QUEUE_DEPTH];
    _Atomic uint32_t  head;    /* Writer 讀取位置 */
    _Atomic uint32_t  tail;    /* Producer 寫入位置 */
} rec_write_queue_t;
```

SPSC queue 使用兩個 atomic index，無 mutex，Writer thread 用 `futex` 或 `eventfd` 喚醒。

### 9.3 MPEG-TS 封裝

```
每個 encoded frame → 切割為 188-byte TS packets
Header：sync byte(0x47) + PID + continuity counter + adaptation field（PCR）
Payload：PES header（含 PTS）+ NAL data
分段邊界：新 segment 從 PAT + PMT + IDR 開始，確保獨立解碼
```

實作依賴：可使用 FFmpeg `libavformat`（輕量），或自行實作基本 TS muxer
（IPcam 常用方案：自實作約 300 行，不引入大型依賴）。

### 9.4 檔案命名規則

```
<output_dir>/<stream_name>_<YYYYMMDD>_<HHMMSS>_<mode>.ts
例：
  cam0_20260424_143000_cont.ts      ← 連續錄影
  cam0_20260424_150230_event.ts     ← 事件錄影
```

---

## 十、對外 API（`rec_engine.h`）

```c
typedef struct rec_engine rec_engine_t;

/* ─── 設定參數 ───────────────────────────────────────────────────── */
typedef struct {
    char              stream_name[VFR_SOCKET_NAME_MAX];
    rec_mode_t        default_mode;       /* 預設模式 */
    rec_schedule_t    schedule;           /* 排程（mode=SCHEDULED 時有效）*/
    uint32_t          pre_record_sec;     /* 預錄秒數（0 = 不預錄）*/
    uint32_t          post_record_sec;    /* 延時秒數 */
    uint32_t          segment_duration_sec;
    uint64_t          segment_size_max;   /* bytes，0 = 只用時間切 */
    uint32_t          ring_buf_size;      /* Circular Buffer 大小（bytes）*/
    char              output_dir[256];    /* 錄影輸出目錄 */
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
 *   呼叫者負責將這些 fd 加入自己的 epoll，並在 EPOLLIN 時呼叫 rec_engine_handle_event()。
 *
 *   目前回傳的 fd（依序，共 3 個）：
 *     [0] VFR consumer eventfd    — 新幀到達通知
 *     [1] AI trigger socket fd    — 跨 process 觸發訊號（Unix abstract socket）
 *     [2] timerfd（1 秒週期）     — POST_WAIT 倒數 + 排程邊界切換
 *
 *   max_fds 建議傳入 REC_MAX_EPOLL_FDS（定義於 rec_defs.h）。
 *   回傳值：實際填入的 fd 數量；< 0 表示錯誤。
 */
int rec_engine_get_epoll_fds(rec_engine_t *eng, int *fds_out, int max_fds);

/*
 * rec_engine_handle_event()：
 *   在 epoll 返回後呼叫，處理對應 fd 上的事件。
 *   涵蓋：新 VFR 幀、AI 觸發訊號、排程計時器。
 *   設計為非阻塞（不得在此函數內呼叫 sleep / 長時間等待）。
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
uint32_t    rec_engine_get_dropped_frames(const rec_engine_t *eng);  /* Write Queue 溢出 */
```

---

## 十一、開發階段（MVP Roadmap）

### Phase R1：Circular Buffer + I-Frame Indexer

**目標**：底層幀緩衝正確性驗證

實作項目：
- `rec_buf.c`：byte-ring 寫入、環繞（wrap-around）、舊幀淘汰
- `rec_buf.h`：`rec_buf_push()`、`rec_buf_extract_from_keyframe()`
- `test/test_rec_buf.c`：

驗收 Checklist：
- [ ] 單元測試：寫入 1000 幀，buffer 滿後舊幀自動淘汰
- [ ] I-Frame 定位：`extract_from_keyframe(t_now - 10s)` 回傳正確範圍
- [ ] Wrap-around：buffer 環繞後索引計算正確，無越界
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

---

### Phase R3：異步寫入 + 分段

**目標**：實際產生可播放的 .ts 檔案

實作項目：
- `rec_writer.c`：Write Queue + Writer thread + MPEG-TS 封裝
- `rec_segment.c`：按時間 / 大小切檔、命名規則
- `test/test_rec_writer.c`：模擬 encoded frames 餵入，驗證輸出檔案

驗收 Checklist：
- [ ] 產生的 .ts 可用 `ffprobe` 正確讀取 duration、codec、PTS
- [ ] 分段切換：在 I-Frame 邊界切換，新 segment 從 PAT+PMT+IDR 開始
- [ ] Write Queue 溢出：Producer 速度 > Writer 速度時，`drop_count` 遞增，不 crash
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
- [ ] 24/7 模式：持續錄影 1 小時，每 10 分鐘自動分段，**無幀遺漏**（前提：磁碟寫入速度足夠，`drop_count == 0`；CI 測試應先斷言此值）
- [ ] 事件模式：AI trigger → 預錄對齊 I-Frame → 錄影 → POST_WAIT → 結束
- [ ] 排程模式：時段 off → 無輸出；時段 continuous → 正常錄影
- [ ] 跨 process 觸發：AI process 送 `vfr_event_msg_t`，Recorder 正確響應
- [ ] VFR Watchdog 整合：Recorder crash 後，VFR server 正確回收 slot
- [ ] Prometheus metrics：`rec_drop_frames`、`rec_written_bytes`、`rec_state` 可 scrape

---

## 十二、嚴格禁止事項

| 禁止行為 | 原因 |
|----------|------|
| 在 event loop thread 呼叫 `write()` | 磁碟 I/O 阻塞，所有 VFR 幀事件卡死 |
| 在 event loop thread 呼叫 `sleep()` / `usleep()` | 同上 |
| 在 event loop thread 對大型 buffer 執行 `memcpy()`（> 數 KB） | ARM DDR 競爭下，15MB memcpy 可達 2~5ms，VFR eventfd 無人 drain；EXTRACT_PRE 只做 index entry enqueue，memcpy 由 Writer thread 執行 |
| Writer thread 直接讀取 `rec_buf_t`（不設 protected_read_offset） | Data race；ring buf 可能在 Writer 讀取期間被 wrap-around 覆蓋；必須先設定保護區間 |
| Write Queue 溢出時 block event loop 等待 | 應 drop frame 並遞增 `drop_count`，不阻塞；VFR 層的 BLOCK_PRODUCER 與 Write Queue drop 是兩個不同層級的機制 |
| 錄影段從非 I-Frame 開始 | 解碼器輸出綠屏或崩潰 |
| 使用 `signal()` 設定 handler | 多執行緒行為未定義；改用 `sigaction()` |
| EXTRACT_PRE 期間持有 VFR slot | `VFR_MAX_CONSUMER_SLOTS=4`，預錄需數百幀，必須在 event loop 中 memcpy 後立即 `vfr_put_frame()` |
| 直接存取 `vfr_server_t` 或 `vfr_pool_t` 內部欄位 | 違反 VFR opaque 設計；改用公開 API |
| Write Queue 使用 mutex（主路徑）| SPSC queue 用 atomic index；mutex 僅用於 flush / destroy |
| `free(item->data)` 在 event loop 端執行 | data 所有權屬 Writer thread；destroy 時必須 join Writer 後才能釋放 engine |
| 直接讀寫 `vfr_frame_t.stride` 當 stream_id | 語意不明；改用 `REC_FRAME_STREAM_ID()` / `REC_FRAME_SET_STREAM_ID()` |
| POST_WAIT / 排程切換靠「每幀到達時檢查時間差」驅動 | 低 fps 或 IDLE 時幀間隔長，計時精度差；應靠 1 秒週期 timerfd |

---

## 十三、關鍵設計決策表

| 項目 | 決策 | 原因 |
|------|------|------|
| 容器格式 | MPEG-TS | 斷電可恢復；無需 moov atom；TS 相容性廣 |
| Circular Buffer 型態 | Byte-ring + Frame Index Table | Encoded frame 變長，不能用固定 slot |
| 預錄記憶體所有權 | Recorder 自有（memcpy 自 VFR mmap）| `VFR_MAX_CONSUMER_SLOTS=4` 不夠存百幀 |
| 寫入執行緒數量 | 1 個 Writer thread | SPSC queue 無 mutex，更簡單可靠 |
| AI 觸發通道 | Unix abstract socket | 跨 process；延續 VFR IPC 慣例 |
| I-Frame 旗標 | `VFR_FLAG_KEY_FRAME` in `vfr_frame_t.flags` | 最小化 VFR 改動，platform adapter 填入 |
| IAV bitstream 取得 | memfd（初期）→ dma-buf（優化）| 先求正確，再求零複製 |
| MPEG-TS 實作 | 自實作基本 TS muxer | 避免引入 FFmpeg 的大型依賴 |
| 排程粒度 | 15 分鐘 | 2 bits × 96 slots = 24 bytes/天，足夠實用 |
| Write Queue 深度 | 256 | 256 × ~40KB avg = ~10MB，可吸收 I/O 抖動 |

---

## 十四、相依套件

| 套件 | 用途 | 最低版本 | 必要性 |
|------|------|----------|--------|
| Linux kernel | pidfd, eventfd, memfd, futex | 5.4+ | 必要 |
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

*文件版本：v1.1 — 2026-04-24*
*v1.1 修正：10 個設計問題（3 🔴 / 4 🟡 / 3 🟢），詳見 issue 審查記錄*
*基於 plan-vfr-cross-platform-v2.md v2.4 設計，VFR 為幀來源層，Recorder 為 VFR 消費者模組*
