# VFR — Video Frame Reader 跨平台影像緩衝架構設計計畫 v2

> **目標**：設計一套以 DMA-BUF fd 為通用貨幣的影像緩衝框架，  
> 讓 RTSP / Recorder / Motion / AI 等消費者在不修改程式碼的前提下，  
> 跨越 Ambarella、SigmaStar、V4L2 等不同晶片平台。
>
> **v2 變更**：整合 CLAUDE.md 五條架構原則、里程碑驗證規範、禁止事項。

---

## 架構總覽

```
┌──────────────────────────────────────────────────────────────┐
│  Layer 5：Consumer SDK                                        │
│  VFR Client Library / Zero-copy Mapper / App Bridges         │
├──────────────────────────────────────────────────────────────┤
│  Layer 4：Metadata & State                                    │
│  SHM State Header（原子計數、Buffer Layout）                  │
├──────────────────────────────────────────────────────────────┤
│  Layer 3：Sync & Flow Control                                 │
│  EventFD / Epoll / Cache Coherency / Backpressure            │
├──────────────────────────────────────────────────────────────┤
│  Layer 2：IPC & Control Plane                                 │
│  SCM_RIGHTS Server / Unix Socket / Session Watchdog          │
├──────────────────────────────────────────────────────────────┤
│  Layer 1：Hardware Ingress                                    │
│  Platform HAL Adapter / Buffer Exporter / Buffer Pool        │
└──────────────────────────────────────────────────────────────┘
         ↓ Amba              ↓ SigmaStar          ↓ V4L2
      /dev/iav            /dev/mi_sys          /dev/videoX
```

---

## 架構決策記錄（原則一）

> 所有重大架構決策附上原因。Claude 無明確技術理由不得自行更改。

| 項目 | 決策 | 原因 |
|------|------|------|
| 通用貨幣 | DMA-BUF fd | 跨 process 零複製的唯一標準機制；fd 可透過 SCM_RIGHTS 傳遞，且 kernel 負責生命週期 |
| IPC 通道 | Unix Abstract Namespace Socket | 不留 filesystem 殘留；`\0/vfr/streamN` 格式天然按 stream 命名隔離 |
| 消費者通知 | EventFD + Epoll | 取代 busy-poll 或 blocking ioctl，CPU 使用率可控；與 Reactor 框架相容 |
| SHM 狀態頭 | 置於每個 stream 的 SHM 起始處 | 消費者可在不建立 socket 的情況下讀取 seq/drop_count，降低 IPC 往返 |
| 降級路徑 | `memfd_create` + memcpy | 老晶片無 DMA-BUF export 時，consumer API 保持不變，只多一次 copy；統一介面比特殊分支更易維護 |
| Backpressure | 三種 Policy，每個 consumer 獨立聲明 | 不同消費者對完整性 vs 即時性的要求不同；全局單一策略會讓某類消費者無法滿足 |
| 容錯偵測 | `pidfd_open()` + epoll POLLIN | 比 heartbeat 更精確；process 結束時 kernel 立即通知，不需要等逾時 |
| Signal Handler | `sigaction()`，不用 `signal()` | `signal()` 在多執行緒環境行為未定義；`sigaction()` 語意明確 |
| 亂數 | `getrandom()`，不用 `rand()` | 安全性亂數（token、session ID）需要密碼學強度，`rand()` 可預測 |
| Buffer Pool 大小 | `vfr_open()` 靜態參數，預設 `VFR_DEFAULT_SLOTS`，上限 `VFR_MAX_SLOTS` | 動態調整需解決 in-flight slot 歸還與 fd 重分配問題，複雜度遠高於收益；靜態配置已覆蓋所有已知場景 |
| Slot 動態調整 | 不實作 | 正確實作需要 slot 借還協議、consumer 端 fd 失效通知，工程成本高；若 pool 不夠，重新 open 即可 |
| VA Cache | 初期不實作 | stale address 風險高；現代 kernel 對已 pin 的 DMA-BUF mmap 成本低；確認成為瓶頸後再優化 |
| 時鐘域同步 | SHM header 預留 `producer_boot_ns`，不實作校正邏輯 | 閉環單 SoC 不需要；跨 SoC / 虛擬化時有需求，預留欄位成本極低，不預留日後 ABI 難加 |
| Log 等級 | 編譯期 `VFR_LOG_LEVEL` macro，預設 INFO | 高 FPS 場景每幀 DEBUG log 影響 timing；用 macro 靜態過濾，release build 零成本 |
| Socket path 安全 | `vfr_open()` 入口強制檢查 `strlen < VFR_SOCKET_NAME_MAX` | abstract namespace 上限 107 bytes，stream name 若來自外部配置有 overflow 風險；一行檢查成本極低 |

---

## 常數唯一定義點（原則二）

所有跨模組共用的 macro 常數集中於 `include/vfr_defs.h`，其他所有 `.h` 以此為第一個 include。

```c
/* include/vfr_defs.h — 唯一定義點 */
#ifndef VFR_DEFS_H
#define VFR_DEFS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VFR_SHM_MAGIC           0x56465231u   /* "VFR1" */
#define VFR_PROTO_VERSION       1u            /* IPC 握手協議版本；struct 新增欄位時遞增 */
#define VFR_DEFAULT_SLOTS       8             /* vfr_open() slot_count 預設值 */
#define VFR_MAX_SLOTS           64            /* slot_count 上限，防止異常配置 */
#define VFR_MAX_CONSUMERS       16
#define VFR_MAX_PLANES          3
#define VFR_SOCKET_PATH_FMT     "\0/vfr/%s"   /* abstract namespace */
#define VFR_SOCKET_NAME_MAX     64            /* stream name 最大長度（bytes），防止 path overflow */
#define VFR_WATCHDOG_TIMEOUT_MS  2000         /* Phase 4 watchdog：偵測 consumer process 死亡的超時 */
#define VFR_MAX_CONSUMER_SLOTS   4            /* 單一 consumer 最多同時持有的 slot 數（當前幀 + 預取）*/

/* vfr_get_frame() flags */
#define VFR_FLAG_NONBLOCK    (1 << 0)
#define VFR_FLAG_NO_CPU_SYNC (1 << 1)

/* vfr_backpressure_t values */
#define VFR_POLICY_DROP_OLDEST    0
#define VFR_POLICY_BLOCK_PRODUCER 1
#define VFR_POLICY_SKIP_SELF      2

/* Log 等級（編譯期靜態過濾，不影響 release build） */
typedef enum {
    VFR_LOG_ERROR = 0,  /* 不可恢復錯誤，永遠輸出 */
    VFR_LOG_WARN  = 1,  /* 可恢復異常（drop、重連） */
    VFR_LOG_INFO  = 2,  /* 生命週期事件（open/close/連線） */
    VFR_LOG_DEBUG = 3,  /* 每幀細節，高頻，僅 debug build 啟用 */
} vfr_log_level_t;

#ifndef VFR_LOG_LEVEL
#  define VFR_LOG_LEVEL VFR_LOG_INFO   /* release 預設：INFO；debug build 傳 -DVFR_LOG_LEVEL=3 */
#endif

#endif /* VFR_DEFS_H */
```

**禁止**在 `vfr_ctx.c`、`vfr_pool.c`、`platform_adapter.h` 等地方重複定義這些值。

---

## 硬體抽象介面（原則三）

所有平台實作藏在函式指標表後面，上層邏輯只操作 `vfr_platform_ops_t`，不操作具體實作。

```c
/* platform/platform_adapter.h */
#include "vfr_defs.h"

typedef struct vfr_frame vfr_frame_t;   /* 前置宣告 */

typedef struct {
    const char *name;                   /* "amba_iav5", "sigmastar_mi", "v4l2" */

    /* 初始化；cfg 來自 SHM Header（格式協商完成後）
     * 成功回傳 0，失敗 -1 */
    int  (*init)    (void **ctx, const vfr_shm_header_t *cfg);

    /* 取一幀；非阻塞
     * 0 = 有幀，1 = 暫無幀（EAGAIN），-1 = 不可恢復錯誤 */
    int  (*get_frame)(void *ctx, vfr_frame_t *out);

    /* 歸還幀（讓 DSP 可再使用該 buffer） */
    void (*put_frame)(void *ctx, vfr_frame_t *frame);

    /* 釋放所有資源 */
    void (*destroy) (void **ctx);

    /* 平台不支援 DMA-BUF 時設為 false → 框架改走 memfd 降級路徑 */
    bool has_native_dma_buf;
} vfr_platform_ops_t;
```

---

## Signal 處理（原則四）

Producer process 和 Server daemon 均使用 `sigaction()`，透過全局旗標優雅退出。

```c
/* 全局退出旗標 */
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int signo) { (void)signo; g_running = 0; }

/* 初始化時設定（使用 sigaction，不用 signal()） */
struct sigaction sa = { .sa_handler = sig_handler };
sigemptyset(&sa.sa_mask);
sigaction(SIGTERM, &sa, NULL);
sigaction(SIGINT,  &sa, NULL);

/* 忽略 SIGPIPE：Unix Socket 寫入 dead consumer 時改由 write() 回傳 EPIPE 處理 */
/* 使用 sigaction 而非 signal()，原則四明確禁止 signal() */
struct sigaction sa_pipe = { .sa_handler = SIG_IGN };
sigemptyset(&sa_pipe.sa_mask);
sigaction(SIGPIPE, &sa_pipe, NULL);

/* 主迴圈 */
while (g_running) {
    /* epoll_wait()，不得用 sleep()/usleep() */
}
```

---

## 資源釋放順序（原則五）

### Producer / Server 端清理順序

```
1. 設定 g_running = 0（防止主迴圈繼續接受新連線）
2. 從 epoll 登出所有 client fd（必須在 close(fd) 之前）
3. 釋放每個 consumer session 的 heap 資料（dispatch list entry）
4. close(client_fd)（觸發 consumer 端 EPOLLHUP，consumer 得到通知）
5. 銷毀 eventfd
6. 從 SHM 登出（munmap → close(shm_fd)）
7. 銷毀 Buffer Pool（按 slot 順序 close(dma_fd)）
8. close(server_listen_fd)
9. free(vfr_ctx)
```

### Consumer 端清理順序（vfr_close）

```
1. 從呼叫者的 epoll 登出 eventfd（必須由呼叫者在 vfr_close 前處理，或框架提供 helper）
2. 若持有 frame（未 put_frame），先呼叫 vfr_put_frame()
3. munmap 所有 mmap 地址（若有 VA cache，全部清除）
4. close(dma_fd)（本地複製的 fd）
5. close(shm_fd)
6. close(unix_socket_fd)
7. free(vfr_ctx)
8. 將呼叫者的指標設為 NULL（由 vfr_close(vfr_ctx_t **ctx) 負責）
```

**重複 close / double-free 防護**：每個資源關閉後立刻設為 -1 / NULL，再次呼叫 `vfr_close` 為 no-op。

---

## 核心資料結構

```c
/* include/vfr.h — 消費者唯一需要引用的標頭 */
#include "vfr_defs.h"

typedef struct {
    int      dma_fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;                       /* fourcc，e.g. V4L2_PIX_FMT_NV12 */
    uint32_t stride;                       /* luma plane pitch，bytes；每幀由 platform adapter 填入 */
    uint32_t buf_size;                     /* 整個 buffer 總大小（bytes）；mmap/munmap 的 length 來源 */
    uint32_t flags;                        /* VFR_FLAG_* bitmask，例如 VFR_FLAG_NO_CPU_SYNC */
    uint32_t plane_offset[VFR_MAX_PLANES]; /* Y / U / V 偏移；每幀由 platform adapter 填入 */
    uint64_t timestamp_ns;
    uint64_t seq_num;
    void    *priv;                         /* platform 私有，消費者不碰 */
} vfr_frame_t;

typedef struct vfr_ctx vfr_ctx_t;

/* 公開 API */
/*
 * vfr_open()：
 *   stream_name  — 長度必須 < VFR_SOCKET_NAME_MAX，否則回傳 NULL
 *   slot_count   — Buffer Pool 大小；傳 0 使用 VFR_DEFAULT_SLOTS；
 *                  建議值：max(預期 consumer 數 × 2, VFR_DEFAULT_SLOTS)
 *                  上限：VFR_MAX_SLOTS（超過回傳 NULL）
 *                  不提供動態調整；需要更多 slot 請重新 open
 */
vfr_ctx_t *vfr_open(const char *stream_name, uint32_t slot_count);
void       vfr_close(vfr_ctx_t **ctx);   /* 將 *ctx 設為 NULL；重複呼叫為 no-op */

int        vfr_get_frame(vfr_ctx_t *ctx, vfr_frame_t *frame, int flags);
/*
 * vfr_put_frame() ownership 語意：
 *   - 呼叫後 frame->dma_fd 由框架負責 close，consumer 不得再使用此 fd
 *   - frame 結構本身由 consumer 管理（stack 變數即可），框架不會 free 它
 *   - 呼叫後不得再對此 frame 呼叫 vfr_map()
 *   - 跨 process 場景：框架發訊號通知 producer 可回收 slot，同時 close 本地 dma_fd 複製
 */
void       vfr_put_frame(vfr_frame_t *frame);

void      *vfr_map(const vfr_frame_t *frame);
void       vfr_unmap(const vfr_frame_t *frame, void *ptr);

/*
 * vfr_get_eventfd() ownership 語意：
 *   - 回傳的 fd 由框架持有，consumer 只可加入自己的 epoll，不得 close() 它
 *   - 呼叫 vfr_close() 前，consumer 必須先從自己的 epoll 登出此 fd
 *     （建議：在 epoll 事件迴圈收到 EPOLLHUP 時主動登出）
 */
int        vfr_get_eventfd(vfr_ctx_t *ctx);
```

---

## 共享記憶體頭部（SHM State Header）

```c
/* include/vfr_defs.h 中定義 VFR_SHM_MAGIC；此結構置於 SHM 起始 */
typedef struct {
    uint32_t magic;                        /* VFR_SHM_MAGIC，版本校驗 */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;                       /* 初始協商值；以 vfr_frame_t 內的每幀值為準 */
    uint32_t plane_offset[VFR_MAX_PLANES]; /* 初始協商值；以 vfr_frame_t 內的每幀值為準 */
    uint32_t slot_count;
    uint64_t producer_boot_ns;             /* producer 啟動時的 CLOCK_REALTIME（nanoseconds）；
                                            * 預留欄位，供跨 SoC / 虛擬化場景做時鐘域 offset 校正；
                                            * 單 SoC 閉環部署可忽略此欄位 */
    _Atomic uint64_t seq;
    _Atomic uint32_t drop_count;
} vfr_shm_header_t;
```

> **注意（Amba IAV canvas resize 等場景）**：SHM header 的 `stride` / `plane_offset` 僅作為初始格式協商參考。  
> 每幀實際的 `stride`、`plane_offset`、`buf_size` **必須由 platform adapter 在 `get_frame()` 時填入 `vfr_frame_t`**，  
> consumer 應以 `vfr_frame_t` 欄位為準，不得快取 SHM header 的值後長期使用。

---

## Cache Coherency 正確順序（勘誤）

> ⚠️ v1 plan 的 code snippet 順序有誤（SYNC 寫在 mmap 之前），已修正如下：

```c
/* vfr_map() 內部正確順序：先 mmap，再 SYNC_START，才讀取 */
void *vfr_map(const vfr_frame_t *frame) {
    /* frame->buf_size 由 platform adapter 每幀填入，是整個 buffer 的總長度 */
    void *ptr = mmap(NULL, frame->buf_size, PROT_READ, MAP_SHARED, frame->dma_fd, 0);
    if (ptr == MAP_FAILED) return NULL;

    if (!(frame->flags & VFR_FLAG_NO_CPU_SYNC)) {
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync);   /* mmap 之後、讀取之前 */
    }
    return ptr;
}

/* vfr_unmap() 內部正確順序：先 SYNC_END，才 munmap */
void vfr_unmap(const vfr_frame_t *frame, void *ptr) {
    if (!(frame->flags & VFR_FLAG_NO_CPU_SYNC)) {
        struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync);   /* munmap 之前 */
    }
    munmap(ptr, frame->buf_size);
}
```

---

## Backpressure Policy

```c
/* 值直接用整數，vfr_defs.h 裡的 #define 已提供相同數值，兩者一致 */
typedef enum {
    VFR_POLICY_DROP_OLDEST    = 0,  /* 即時性優先（RTSP、Preview） */
    VFR_POLICY_BLOCK_PRODUCER = 1,  /* 完整性優先（Recorder） */
    VFR_POLICY_SKIP_SELF      = 2,  /* 自己跳過（AI inference） */
} vfr_backpressure_t;
```

---

## 目錄結構

```
vfr/
├── include/
│   ├── vfr_defs.h              # 唯一常數定義點（原則二）
│   └── vfr.h                   # 消費者唯一引用的標頭
│
├── core/
│   ├── vfr_ctx.c               # context 管理、stream 生命週期
│   ├── vfr_pool.c              # Buffer Pool Manager（Slot Allocator）
│   ├── vfr_shm.c               # SHM State Header 讀寫
│   └── vfr_sync.c              # EventFD、Epoll、Cache Coherency
│
├── ipc/
│   ├── vfr_server.c            # SCM_RIGHTS Server（FD 分發）
│   ├── vfr_client.c            # FD Receiver（recvmsg）
│   └── vfr_watchdog.c          # Session Watchdog（pidfd_open）
│
├── platform/
│   ├── platform_adapter.h      # 純虛擬介面（原則三）
│   ├── amba/
│   │   └── amba_adapter.c      # IAV ioctl 實作
│   ├── sigmastar/
│   │   └── ss_adapter.c        # MI_SYS API 實作
│   └── v4l2/
│       └── v4l2_adapter.c      # V4L2 DMABUF 實作
│
├── sdk/
│   ├── vfr_map.c               # Zero-copy Mapper（含 DMA_BUF_IOCTL_SYNC）
│   ├── bridge_opencv.c         # cv::Mat 構造（Phase 5）
│   └── bridge_gstreamer.c      # GStreamer src element（Phase 5）
│
└── test/
    ├── test_single_proc.c      # Phase 1 驗證
    ├── test_ipc.c              # Phase 2 驗證
    ├── test_multicast.c        # Phase 3 驗證
    └── test_crash_recovery.c   # Phase 4 驗證
```

---

## 實作階段

### Phase 1：MVP — 單 Process 影像讀取

**目標**：Producer 透過 Platform Adapter 取得 frame，Consumer 在同一 process 透過 vfr API 讀取並 mmap，驗證 zero-copy 正確。

> **實作前必讀**：`POOL_DESIGN.md` §一（Slot 狀態機）、§四（Amba Adapter 邊界）

**實作項目**（每項須同時交付 `.h` + `.c` + unit test）：
- [ ] `include/vfr_defs.h`：所有 macro 常數
- [ ] `vfr_shm_header_t` 定義與初始化
- [ ] `vfr_platform_ops_t` 介面定義
- [ ] `amba_adapter.c`：封裝 `IAV_IOC_QUERY_DESC`，export `dma_buf_fd`（欄位名稱依 SDK header，見 `POOL_DESIGN.md` §四）
- [ ] `vfr_pool.c`：Slot Allocator，依 `POOL_DESIGN.md` §一 的狀態機與 refcount 設計實作
- [ ] `vfr_map.c`：`mmap(dma_fd)` / `munmap`，加 `DMA_BUF_IOCTL_SYNC`（正確順序）
- [ ] `vfr_get_frame()` / `vfr_put_frame()` 基本實作
- [ ] `test_single_proc.c`：讀 10 幀，dump 到 `/tmp/frame_xxx.yuv`

**驗收條件**：
- 讀出的 YUV frame 用 `ffplay` 可正確顯示
- `valgrind --leak-check=full` 無 leak、無 memory error
- 無 `memcpy`：靜態驗證用 `objdump -d test_single_proc | grep -c 'call.*memcpy'` 應為 0；動態驗證用 `perf stat -e cache-misses` 比較有無 memcpy 版本的差異（`/proc/PID/io` 的 write_bytes 是 page cache syscall 量，量不到 memcpy）

**Phase 1 驗證 Checklist**：

| 項目 | 驗證方式 |
|------|----------|
| 3.1 Build | `make test_single_proc` 獨立編譯 |
| 3.1 Standalone | 無需完整系統，直接 `./test_single_proc` |
| 3.1 Mock | 平台依賴替換為預錄 binary file |
| 3.2 空輸入 | `vfr_get_frame(NULL, ...)` / `vfr_put_frame(NULL)` 不 crash |
| 3.2 非法 fd | dma_fd = -1 時 `vfr_map()` 回傳 NULL |
| 3.2 Buffer 邊界 | slot_count = 1 極限情境 |
| 3.3 double-free | 連續兩次 `vfr_close(&ctx)` 為 no-op |
| 3.4 valgrind | `valgrind --leak-check=full` 無 error |
| 3.5 malloc fail | mock `malloc` 回傳 NULL，vfr_pool 初始化應回傳錯誤 |
| 3.6 debug log | `DEBUG=1` 下可見每幀 seq_num 與 timestamp |

---

### Phase 2：IPC — 跨 Process FD 傳遞

**目標**：Producer process 透過 Unix socket 將 `dma_fd` 傳給 Consumer process，兩者 mmap 同一塊 DSP 記憶體。

**連線握手協議（含版本號）**：

```c
/* ipc/vfr_server.h — 握手訊息，client 連線後 server 第一個發送 */
typedef struct {
    uint32_t magic;          /* VFR_SHM_MAGIC，快速過濾非 VFR 連線 */
    uint16_t proto_version;  /* 目前為 1；日後欄位新增時遞增 */
    uint16_t header_size;    /* sizeof(vfr_shm_header_t)；client 據此判斷是否相容 */
} vfr_handshake_t;
```

Consumer 收到 `vfr_handshake_t` 後：
1. 驗證 `magic == VFR_SHM_MAGIC`，不符直接關閉連線
2. 比對 `proto_version`：不相容版本拒絕連線並輸出明確 log（`%s:%d proto_version mismatch: got %u, expect %u`）
3. 用 `header_size` 決定要讀取的 `vfr_shm_header_t` 長度（向前相容：新欄位在尾部追加，舊 consumer 只讀已知範圍）

> **實作前必讀**：`POOL_DESIGN.md` §二（SCM_RIGHTS FD 生命週期）、§三（Dispatch 機制）

**實作項目**（每項須同時交付 `.h` + `.c` + unit test）：
- [ ] `vfr_server.c`：Abstract namespace socket，`sendmsg()` + `SCM_RIGHTS`（fd 生命週期見 `POOL_DESIGN.md` §二）
- [ ] `vfr_client.c`：`recvmsg()` 取得本地有效 fd，`vfr_open()` 封裝
- [ ] 連線協議：client 連接時 server 先發 `vfr_handshake_t`，再發 `vfr_shm_header_t`
- [ ] Dispatch list 依 `POOL_DESIGN.md` §三 的 `vfr_consumer_session_t` + `vfr_dispatch_list_t` 設計實作
- [ ] `vfr_open()` 入口：`strlen(stream_name) >= VFR_SOCKET_NAME_MAX` 時立即回傳 NULL 並 log 錯誤（防止 abstract socket path overflow）
- [ ] Signal handler：producer 收到 SIGTERM/SIGINT 後優雅退出，按原則五順序清理
- [ ] `test_ipc.c`：producer / consumer 兩個獨立 binary
- [ ] （選做）`SO_PEERCRED` 驗證 consumer UID：閉環工控環境不強制，但若 stream name 由外部配置時建議啟用

**驗收條件**：
- producer 在 dispatch 前於 frame buffer 起始位置寫入 magic number（`*(uint32_t*)mmap_ptr = 0xDEADBEEF`），consumer mmap 後讀到相同值，證明 zero-copy（`pmap` 只顯示虛擬地址，無法驗證實體頁共享）
- producer crash 後 consumer 不 hang（fd 被 kernel 關閉，consumer 得到錯誤碼）
- producer 收到 SIGTERM，按釋放順序清理後退出，`valgrind` 無 leak

**Phase 2 驗證 Checklist**：

| 項目 | 驗證方式 |
|------|----------|
| 3.1 Build | `make test_ipc`，producer / consumer 各自獨立編譯 |
| 3.2 連線中斷 | `nc -U /tmp/vfr_test` 傳垃圾資料，server 不 crash |
| 3.2 部分讀取 | `recvmsg` 分兩次讀，驗證 header reassembly |
| 3.2 stream name 邊界 | 長度等於 `VFR_SOCKET_NAME_MAX` 時回傳 NULL；長度 `VFR_SOCKET_NAME_MAX - 1` 時正常 open |
| 3.2 proto version 不符 | consumer 傳舊版 `proto_version`，server 拒絕並輸出錯誤 log |
| 3.3 重複連線 | consumer 連線 → 斷線 → 重連，不得累積 fd |
| 3.4 fd leak | `/proc/PID/fd` 計數在 consumer 斷線後恢復 |
| 3.5 consumer 強制 kill | `kill -9 consumer_pid`，producer 繼續正常出幀 |
| 3.6 error log | `EPIPE` / `ECONNRESET` 事件有 `__func__:__LINE__` log |

---

### Phase 3：Sync — 低延遲多端消費

**目標**：多個 consumer 同時消費，延遲 < 1 frame（16ms @ 60fps），快取一致性正確。

> **實作前必讀**：`POOL_DESIGN.md` §三（Dispatch 偽碼、每 consumer 獨立 eventfd、DROP_OLDEST 步驟）

**實作項目**（每項須同時交付 `.h` + `.c` + unit test）：
- [ ] `vfr_sync.c`：每個 consumer session 各有一個 `eventfd`（不共用），producer dispatch 後對每個 consumer 各寫一次（設計細節見 `POOL_DESIGN.md` §三）
- [ ] Consumer 用 `epoll_wait(eventfd)` 取代 busy-polling（**禁止使用 `sleep()` 在主迴圈**）
- [ ] `DMA_BUF_IOCTL_SYNC` wrapper（已在 Phase 1 `vfr_map.c` 中，Phase 3 驗證多端場景）
- [ ] Backpressure Policy 實作（DROP_OLDEST / BLOCK_PRODUCER / SKIP_SELF）
- [ ] `test_multicast.c`：3 個 consumer（RTSP + Recorder + Motion），量測各端延遲

**驗收條件**：
- 3 個 consumer 同時跑，端對端延遲 < 16ms，量測方式：
  ```c
  /* consumer 收到 eventfd 通知後，立即取時間戳 */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t receive_ns = (uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec;
  uint64_t latency_ns = receive_ns - frame.timestamp_ns;
  /* 前提：producer 的 timestamp_ns 與 consumer 使用同一個 clock domain（CLOCK_MONOTONIC）
   * 若跨 SoC 時鐘域，需先做 clock offset 校正 */
  ```
- Slow consumer（人工加 sleep）不影響其他 consumer 的延遲（DROP_OLDEST policy）
- `hexdump` 驗證 CPU 讀取的 YUV 資料與 GPU 處理結果一致（Cache Sync 正確）

**Phase 3 驗證 Checklist**：

| 項目 | 驗證方式 |
|------|----------|
| 3.2 Backpressure 邊界 | 全部 slot 被佔滿時，DROP_OLDEST 正確覆寫最舊 slot |
| 3.3 Policy 轉換 | 同一 stream 上混合三種 policy 的消費者，各自行為符合規格 |
| 3.4 CPU 穩定 | `top` 觀察 3 consumer 同跑時 CPU usage，不得有 busy loop |
| 3.4 ASan | `-fsanitize=address` 無 error |
| 3.5 BLOCK_PRODUCER 逾時 | BLOCK_PRODUCER consumer 卡住，producer 應有**獨立的 slot wait timeout**（建議 1 frame 時間，即 `1000ms / fps`），逾時後對該 consumer 降級為 DROP_OLDEST；`VFR_WATCHDOG_TIMEOUT_MS` 是 Phase 4 watchdog 偵測 process 死亡的超時，兩者為不同機制，不得混用 |
| 3.6 drop_count | SHM header `drop_count` 在 DROP_OLDEST 觸發時正確遞增 |

---

### Phase 4：Robust — 容錯與資源回收

**目標**：任意 consumer crash 或卡住，不影響 producer 和其他 consumer；資源完整回收。

**實作項目**（每項須同時交付 `.h` + `.c` + unit test）：
- [ ] `vfr_watchdog.c`：`pidfd_open()` 監控 consumer PID，偵測 POLLIN 事件
- [ ] Dead consumer 清理：從 dispatch list 移除，回收 slot 佔用（按原則五順序）
- [ ] `vfr_shm_header_t.drop_count` 監控介面（供 metrics 系統讀取）
- [ ] `memfd_create` 降級路徑（`has_native_dma_buf = false` 時）
- [ ] `test_crash_recovery.c`：consumer `kill -9`，驗證 producer 恢復正常，slot 全數釋放

**Dead consumer 清理順序（原則五）**：
```
1. 標記 consumer session tombstone（防止後續邏輯誤用）
2. 從 epoll 登出 consumer 的 eventfd / pidfd（必須在 close 之前）
3. 釋放 dispatch list entry 的 heap 資料
4. close(client_fd)
5. 歸還此 consumer 持有的所有 slot（close 本地 dma_fd 複製）
6. 從 consumer 陣列清除指標
7. free(session 結構)
```

**驗收條件**：
- consumer `kill -9` 後，`/proc/PID/fd` 確認 dma_fd 被 kernel 關閉
- Slot 在 watchdog 觸發後 `VFR_WATCHDOG_TIMEOUT_MS` ms 內歸還 pool
- 降級路徑（memfd）下 consumer API 呼叫方式與 DMA-BUF 路徑完全相同

**Phase 4 驗證 Checklist**：

| 項目 | 驗證方式 |
|------|----------|
| 3.2 重複 teardown | `vfr_close` 連呼兩次，第二次為 no-op |
| 3.3 reset 後狀態歸零 | watchdog 清理後 slot 計數恢復、drop_count 不誤增 |
| 3.4 fd leak | kill consumer 後 `/proc/producer_pid/fd` 計數穩定 |
| 3.4 長跑 | > 1hr 無 memory growth（`/proc/PID/status` VmRSS 監控） |
| 3.5 錯誤路徑 | memfd 路徑下 `malloc` fail，正確 partial cleanup |
| 3.8 長跑穩定 | > 1hr 無 crash、無 fd/handle leak |

---

### Phase 5：Ecosystem（按需實作）

**目標**：提供便利性工具，讓 AI / 串流模組快速接入。

**項目（優先度由高到低）**：
- [ ] OpenCV Bridge：`vfr_to_mat(frame, mat)` 直接構造 `cv::Mat`（零複製）
- [ ] GStreamer Bridge：`vfrsrc` element，讓 frame 進入 GStreamer pipeline
- [ ] Prometheus metrics endpoint：`drop_count`、latency histogram、**slot usage histogram**（`slot_used / slot_total` 時序分佈，用於定位 pool 大小是否足夠）
- [ ] Registry Table：動態 stream 發現（仿 ZeroMQ 的 ServiceBinder 模式）

> **log 等級提醒**：Phase 5 metrics 輸出應受 `VFR_LOG_LEVEL` 控制，`INFO` 等級只輸出彙整統計，`DEBUG` 等級才輸出每幀數值，避免高 FPS 場景 log 量影響 timing。

---

## 嚴格禁止事項（對應 CLAUDE.md Part 4）

| 禁止行為 | 對應原則 | 後果 |
|----------|----------|------|
| 在各 `.h` / `.c` 重複定義 macro 常數 | 原則二 | 多重定義衝突，修改時容易遺漏 |
| 直接操作硬體不透過 `vfr_platform_ops_t` | 原則三 | 無法 mock，無法在沒有硬體時測試 |
| 資源釋放不按原則五規定的順序 | 原則五 | use-after-free、event 繼續觸發已釋放資源 |
| 在主迴圈使用 `sleep()` / `usleep()` | 原則四 | 阻塞 epoll，所有 consumer 事件卡死 |
| 錯誤路徑只 return -1，不釋放已取得資源 | 原則五 | partial cleanup 導致 leak |
| 使用 `rand()` 生成 token / session ID | 原則四 | 可預測，改用 `getrandom()` |
| 使用 `signal()` 設定 handler | 原則四 | 多執行緒環境行為未定義 |
| 里程碑驗收未通過就進入下一個 | Part 2 | 底層 bug 在上層才爆，debug 成本指數上升 |
| 一次生成整個專案再測試 | Part 2 | 錯誤連鎖污染，無法定位根因 |
| busy-poll 等待新幀 | 原則四 | CPU 浪費；改用 `epoll_wait(eventfd)` |

---

## 各晶片驗證計畫

| 晶片 | DMA-BUF | `has_native_dma_buf` | 進入 Phase |
|------|---------|----------------------|-----------|
| Ambarella CV5 / CV52 (iav5) | 支援 | `true` | Phase 1 |
| Ambarella CV72 (iav6) | 支援 | `true` | Phase 1 |
| SigmaStar SSC338Q | 支援 | `true` | Phase 2 後 |
| 任意 V4L2 平台 | 原生支援 | `true` | Phase 1 |
| 老晶片（無 DMA-BUF） | 降級 | `false` | Phase 4 |

---

## 相依套件

| 套件 | 用途 | 最低版本 |
|------|------|----------|
| Linux kernel | DMA-BUF, eventfd, pidfd, memfd | 5.4+ |
| libc | `memfd_create`, `SCM_RIGHTS`, `getrandom` | glibc 2.27+ |
| OpenCV | Bridge（Phase 5，可選） | 3.4+ |
| GStreamer | Bridge（Phase 5，可選） | 1.16+ |

---

*文件版本：v2.4 — 2026-04-22*  
*v2.0 變更：整合 CLAUDE.md 五條架構原則、里程碑驗證 Checklist、禁止事項；修正 Cache Coherency 順序錯誤；新增 `vfr_defs.h` 常數定義；補充 Signal 處理與資源釋放順序*  
*v2.1 變更：vfr_frame_t 補 `flags` 與 `buf_size` 欄位；vfr_map/unmap 改用 `frame->buf_size`；補充 vfr_put_frame / vfr_get_eventfd ownership 語意；SHM header stride/plane_offset 標注為協商初始值；新增 vfr_handshake_t 與 VFR_PROTO_VERSION；修正 Phase 1 memcpy 驗證方式；補充 Phase 3 延遲量測 code snippet；釐清 BLOCK_PRODUCER timeout 與 watchdog 為不同機制*  
*v2.2 變更：vfr_open() 改為靜態 slot_count 參數（不做動態調整）；vfr_defs.h 加 VFR_MAX_SLOTS / VFR_SOCKET_NAME_MAX / VFR_LOG_LEVEL enum；SHM header 加 producer_boot_ns 預留欄位；Phase 2 加 socket path 長度驗證與 SO_PEERCRED 選做項目；Phase 5 補 slot usage histogram 與 log 等級說明；決策表新增六條*  
*v2.3 變更：新增 POOL_DESIGN.md（Slot 狀態機、refcount、SCM_RIGHTS fd 生命週期、Dispatch 偽碼、Amba Adapter ioctl 邊界）；Phase 1/2/3 實作項目加入對應的 POOL_DESIGN.md 章節引用*  
*v2.4 變更：修正 Backpressure enum 自我指派（→ 直接整數值）；SIGPIPE 改用 sigaction 取代 signal()（原則四）；Phase 2 驗收改用 magic number 驗證零複製（pmap 無法驗證實體頁）；POOL_DESIGN.md v1.1 同步修正（見該文件 changelog）*
