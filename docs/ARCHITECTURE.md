# VFR + Recording Engine 完整架構解說
> **比喻主軸：智慧停車場管理系統**

---

## 目錄

1. [比喻體系速查表](#比喻體系速查表)
2. [系統全局架構圖](#系統全局架構圖)
3. [層一：基礎規格書（vfr\_defs.h / vfr.h）](#層一基礎規格書)
4. [層二：土地選擇器（Platform Adapter）](#層二土地選擇器platform-adapter)
5. [層三：停車場核心](#層三停車場核心)
   - [Buffer Pool（vfr\_pool）](#buffer-poolvfr_pool)
   - [鈴鐺系統（vfr\_sync）](#鈴鐺系統vfr_sync)
6. [層四：IPC 通訊協議（vfr\_ipc\_types）](#層四ipc-通訊協議)
7. [層五：服務端（Producer 側）](#層五服務端producer-側)
   - [停車場管理總部（vfr\_server）](#停車場管理總部vfr_server)
   - [看板系統（vfr\_registry）](#看板系統vfr_registry)
   - [心跳監測儀（vfr\_watchdog）](#心跳監測儀vfr_watchdog)
8. [層六：客戶端（Consumer 側）](#層六客戶端consumer-側)
9. [層七：錄影引擎（Recording Engine）](#層七錄影引擎recording-engine)
   - [錄影部門總管（rec\_engine）](#錄影部門總管rec_engine)
   - [值班狀態機（rec\_state）](#值班狀態機rec_state)
   - [行車紀錄器（rec\_buf）](#行車紀錄器rec_buf)
   - [存檔工人（rec\_writer）](#存檔工人rec_writer)
   - [捲宗分段（rec\_segment）](#捲宗分段rec_segment)
   - [MPEG-TS 封裝機（rec\_ts\_mux）](#mpeg-ts-封裝機rec_ts_mux)
   - [班表系統（rec\_schedule）](#班表系統rec_schedule)
   - [AI 報警接收台（rec\_trigger）](#ai-報警接收台rec_trigger)
   - [防誤報過濾器（rec\_debounce）](#防誤報過濾器rec_debounce)
   - [即時報表系統（rec\_metrics）](#即時報表系統rec_metrics)
10. [層八：SDK 監控儀表板（vfr\_metrics）](#層八sdk-監控儀表板)
11. [完整生命週期流程](#完整生命週期流程)
12. [常數速查表](#常數速查表)

---

## 比喻體系速查表

| 技術名詞 | 停車場比喻 | 一句話說明 |
|---|---|---|
| **Buffer Pool** | 停車場 + 全部車位 | 共享記憶體空間，存放影像 Buffer |
| **Slot** | 停車位 | 一格 DMA Buffer，可放一張影像 |
| **DMA-BUF fd** | 車位鑰匙 | 指向硬體記憶體的 File Descriptor |
| **Producer** | 停車場生產者（相機/DSP） | 不斷把新車停進空位 |
| **Consumer** | 停車場租車客戶 | 借走鑰匙去使用影像的 App |
| **Registry** | 停車場入口看板（黃頁） | 讓客戶知道有哪些影像流可以連 |
| **Server** | 停車場管理總部 | 接受客戶連線、分發鑰匙、監控 |
| **Client** | 客戶服務介面 | Consumer App 與 Server 溝通的橋梁 |
| **eventfd** | 呼叫器 / 門鈴 | 有新幀時通知 Consumer，零 CPU 輪詢 |
| **SCM\_RIGHTS** | 時空傳送盒 | 跨 Process 傳遞 FD 的 Unix 機制 |
| **pidfd** | 心跳監測儀 | 偵測 Consumer Process 是否還活著 |
| **Watchdog** | 緊急救護車 | Consumer 猝死時立刻清理占用資源 |
| **Backpressure** | 車位滿時的疏導策略 | 決定新車停不進去時怎麼辦 |
| **Tombstone** | 黑盒子 / 行車紀錄器 | Process 崩潰前留下的除錯資訊 |
| **rec\_buf** | 環形行車記錄器 | 最近 N 秒的影像，用於事件前置錄影 |
| **rec\_engine** | 錄影部門總管 | 整合所有錄影子模組的主控制器 |
| **rec\_state** | 值班警衛狀態機 | 控制「閒置/等 I-Frame/錄影/結尾」的切換 |
| **rec\_writer** | 搬運工 / 存檔員 | 獨立 Thread，把資料寫進 .ts 檔案 |
| **rec\_segment** | 卷宗管理員 | 決定何時切換新檔案（時間/大小） |
| **rec\_ts\_mux** | 集裝箱打包機 | 把 H.264/H.265 封裝成 MPEG-TS 格式 |
| **rec\_schedule** | 排班表 | 決定哪個時段要錄影 |
| **rec\_trigger** | AI 報警接收台 | 接收 AI 模型的「發現可疑人物」通知 |
| **rec\_debounce** | 防誤報濾波器 | 過濾 500ms 內重複的觸發訊號 |
| **rec\_metrics** | 錄影部門報表 | Prometheus 格式的錄影健康度輸出 |
| **vfr\_metrics** | 停車場儀表板 | Prometheus 格式的 VFR 吞吐量輸出 |

---

## 系統全局架構圖

```
┌─────────────────────────────────────────────────────────────────────┐
│                        整個 IPCam 系統                               │
│                                                                     │
│  ┌──────────────────────┐        ┌─────────────────────────────┐   │
│  │   Registry Daemon    │        │       VFR Server            │   │
│  │  (停車場入口看板)     │        │      (停車場管理總部)         │   │
│  │  vfr_registry_serve  │        │  vfr_server_create()        │   │
│  │  _forever()          │        │  vfr_server_handle_events() │   │
│  └──────────┬───────────┘        │  vfr_server_produce()       │   │
│             │ 登記 stream 名稱    │                             │   │
│             │ (register)         │  ┌──────────────────────┐   │   │
│  ┌──────────▼───────────┐        │  │   Buffer Pool        │   │   │
│  │   Producer (Camera)  │──────▶│  │  (停車場車位)         │   │   │
│  │  Platform Adapter    │ 取幀   │  │  vfr_pool_acquire()  │   │   │
│  │  get_frame() ────────┼────▶  │  │  vfr_pool_dispatch() │   │   │
│  └──────────────────────┘        │  └──────────────────────┘   │   │
│                                  │                             │   │
│                                  │  SCM_RIGHTS 傳遞 DMA-BUF fd │   │
│                                  │  eventfd 通知 (呼叫器)       │   │
│                                  │           │                 │   │
│                                  └───────────┼─────────────────┘   │
│                                              │                     │
│              ┌───────────────────────────────┤                     │
│              ▼                               ▼                     │
│  ┌───────────────────┐         ┌─────────────────────────┐        │
│  │   Preview App     │         │   Recording Engine      │        │
│  │  (預覽客戶)        │         │   (錄影部門)             │        │
│  │  vfr_open()       │         │  rec_engine_create()    │        │
│  │  vfr_get_frame()  │         │  rec_engine_push_frame()│        │
│  │  vfr_put_frame()  │         │                         │        │
│  └───────────────────┘         │  ┌──────────────────┐   │        │
│                                │  │  rec_buf         │   │        │
│                                │  │  (行車記錄器)     │   │        │
│                                │  └──────────────────┘   │        │
│                                │  ┌──────────────────┐   │        │
│                                │  │  rec_writer      │   │        │
│                                │  │  (存檔工人)       │──────────▶ .ts 檔案  │
│                                │  └──────────────────┘   │        │
│                                └─────────────────────────┘        │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 層一：基礎規格書

**檔案：** `include/vfr_defs.h`、`include/vfr.h`

> **停車場比喻**：就像停車場建設前必須先確定的「工程規格書」。
> 車位大小（Buffer 大小）、最多幾個客戶（`VFR_MAX_CONSUMERS`）、鑰匙規格（FD 傳遞協議）……全部在這裡定義。

### 關鍵常數

| 常數 | 值 | 意義 |
|---|---|---|
| `VFR_SHM_MAGIC` | `0x56465231` ("VFR1") | 防止非 VFR 連線混入的「暗語」 |
| `VFR_PROTO_VERSION` | `2` | IPC 握手協議版本，版本不符直接拒絕 |
| `VFR_DEFAULT_SLOTS` | `8` | 預設車位數量 |
| `VFR_MAX_SLOTS` | `64` | 車位數量上限 |
| `VFR_MAX_CONSUMERS` | `16` | 最多同時 16 個租車客戶 |
| `VFR_MAX_CONSUMER_SLOTS` | `4` | 每位客戶最多同時持有 4 張鑰匙 |
| `VFR_BLOCK_PRODUCER_TIMEOUT_MS` | `33ms` | BLOCK_PRODUCER 最長等待（一幀 @30fps） |
| `VFR_WATCHDOG_TIMEOUT_MS` | `2000ms` | 心跳監測超時（保底機制） |

### 背壓策略（Backpressure Policy）

當停車場車位全滿、新車進不來時，有三種處置辦法：

| 策略 | 停車場比喻 | 適用場景 |
|---|---|---|
| `VFR_BP_DROP_OLDEST` | **拖走最舊的車**：強制把客戶手上最舊的幀收回，讓新幀進來 | RTSP 預覽（要即時性，可以掉幀） |
| `VFR_BP_BLOCK_PRODUCER` | **稍等**：管理員等客戶還車（最多 33ms），才讓下一台新車入場 | Recorder（不能掉幀，寧願慢） |
| `VFR_BP_SKIP_SELF` | **跳過此客戶**：這位客戶這幀先不給，下一幀再說 | AI Inference（可以跳幀） |

### 公開 API（Consumer 唯一需要的 5 個函式）

```c
vfr_ctx_t *vfr_open(const char *stream_name, uint32_t slot_count);
```
- **停車場比喻**：「辦理停車場會員證」。Consumer 呼叫後取得一個不透明的 Context，
  框架內部連到 Server、完成握手、取得呼叫器（eventfd）。
- `stream_name`：停車場名稱（如 `"cam0_main"`），長度必須 < 64 bytes。
- `slot_count`：預申請的車位數，傳 0 用預設值（8）。

```c
void vfr_close(vfr_ctx_t **ctx);
```
- **停車場比喻**：「退卡、還清所有鑰匙、離開停車場」。
- 注意：呼叫前需先把 eventfd 從自己的 epoll 登出，否則會有 dangling fd。

```c
int vfr_get_frame(vfr_ctx_t *ctx, vfr_frame_t *frame, int flags);
```
- **停車場比喻**：「去窗口拿鑰匙（取車）」。
- `VFR_FLAG_NONBLOCK`：沒新車就立刻回傳（非阻塞）；否則阻塞等待。
- 成功後 `frame->dma_fd` 就是這張鑰匙（DMA-BUF fd），可以直接 mmap 讀影像。

```c
void vfr_put_frame(vfr_frame_t *frame);
```
- **停車場比喻**：「還車鑰匙」。Consumer 用完影像後必須呼叫，否則停車位永久被占。
- 呼叫後框架會關閉 `frame->dma_fd`，Consumer 不得再存取此 fd。

```c
void *vfr_map(const vfr_frame_t *frame);
void  vfr_unmap(const vfr_frame_t *frame, void *ptr);
```
- **停車場比喻**：「開車門查看車內物品」。
- `vfr_map()` 用 `mmap` 把 DMA buffer 映射到 CPU 可讀的虛擬地址，並自動做 cache sync（`DMA_BUF_IOCTL_SYNC`）。
- `vfr_unmap()` 結束時 cache sync 並 munmap。

```c
int vfr_get_eventfd(vfr_ctx_t *ctx);
```
- **停車場比喻**：「拿到屬於自己的呼叫器」。
- 這個 fd 加入自己的 `epoll` 後，有新幀時 kernel 會直接喚醒 Consumer，不需 CPU 輪詢。
- **注意**：fd 由框架持有，Consumer 不得 `close()` 它。

---

## 層二：土地選擇器（Platform Adapter）

**檔案：** `platform/platform_adapter.h`、`platform/amba/amba_adapter.c`、`platform/mock/mock_adapter.c`

> **停車場比喻**：「選擇停車場要蓋在哪塊土地上」。
> 同樣的停車場設計，可以蓋在 Ambarella 的高速硬體土地上，也可以蓋在電腦模擬的 mock 土地上。
> 上層管理邏輯完全不知道下面是什麼土地——只透過這份**操作介面（ops）**下指令。

### `vfr_platform_ops_t`（操作介面函式表）

這是一個**純虛擬介面（vtable）**，所有平台都必須實作以下 4 個函式指標：

```c
int  (*init)    (void **ctx, const vfr_shm_header_t *cfg);
```
- 初始化硬體資源。Ambarella 版會開啟 `/dev/iav`；Mock 版建立 memfd。

```c
int  (*get_frame)(void *ctx, vfr_frame_t *out);
```
- **非阻塞**取一幀。
- 回傳 `0` = 成功（`out` 已填好 DMA fd、寬高、時間戳）；`1` = 暫無新幀（EAGAIN）；`-1` = 嚴重錯誤。

```c
void (*put_frame)(void *ctx, vfr_frame_t *frame);
```
- 歸還幀給平台（DSP 得以重用該硬體 buffer）。Mock 版只是 `close(dma_fd)`。

```c
void (*destroy) (void **ctx);
```
- 釋放所有平台資源。必須是冪等操作（傳入 NULL 時為 no-op）。

### 平台選擇函式

```c
const vfr_platform_ops_t *vfr_select_platform(void);
```
- 讀取環境變數 `VFR_PLATFORM`：
  - `"amba"` → 使用 `vfr_get_amba_ops()`（Ambarella IAV driver）
  - 其他或未設 → 使用 `vfr_get_mock_ops()`（開發/測試用 mock）

---

## 層三：停車場核心

### Buffer Pool（`vfr_pool`）

**檔案：** `core/vfr_pool.h`、`core/vfr_pool.c`

> **停車場比喻**：這是**實體停車場的車位管理系統**。
> 記錄哪些車位是空的（FREE）、哪些正在停車中（FILLING）、哪些已出借給客戶（IN\_FLIGHT）。
> 每個 Slot 就是一格停車位，對應一塊 DMA 硬體記憶體。

**Slot 狀態機：**
```
FREE ──acquire()──▶ FILLING ──dispatch()──▶ IN_FLIGHT ──refcount=0──▶ FREE
       (劃定車位)   (正在停車)  (鑰匙已發出)   (所有人還了鑰匙)
```

#### 生命週期函式

```c
vfr_pool_t *vfr_pool_create(const vfr_platform_ops_t *ops,
                             uint32_t slot_count,
                             const vfr_shm_header_t *cfg);
```
- **比喻**：「蓋好整座停車場，劃出所有車位」。
- 呼叫 `platform->init()` 初始化硬體，配置 `slot_count` 個 Slot 結構。

```c
void vfr_pool_destroy(vfr_pool_t **pool);
```
- **比喻**：「拆除停車場」。釋放所有 Slot 的 DMA buffer，`*pool` 設為 NULL。

#### 生產者端操作

```c
int vfr_pool_acquire(vfr_pool_t *pool, uint32_t *out_idx);
```
- **比喻**：「找一個空車位，叫新車停進去」。
- 找一個 FREE Slot → 呼叫 `platform->get_frame()` 填入影像 → 狀態改為 FILLING。
- 回傳 `out_idx`：這個 Slot 的編號，之後 dispatch 要用。
- 回傳 `1`（EAGAIN）：平台說現在沒新幀，叫你等等再試。

```c
void vfr_pool_cancel_acquire(vfr_pool_t *pool, uint32_t slot_idx);
```
- **比喻**：「叫進來的車又叫它出去」。
- 已 `acquire()` 但發現沒有任何 Consumer 需要時，直接丟棄這幀，Slot 還回 FREE。

```c
int vfr_pool_begin_dispatch(vfr_pool_t *pool, uint32_t slot_idx, uint32_t n_consumers);
```
- **比喻**：「發出庫存通知：這格車位的鑰匙，總共要發給 N 個客戶」。
- 在發出任何 `sendmsg` **之前**必須呼叫，一次性設定 `refcount = n`。
- **為什麼重要？** 如果先發給 Consumer A，A 極快還車使 refcount 歸零，B 的鑰匙還沒發出去，
  系統就會以為這幀已結束，造成記憶體被提前清空。先設好 N 就完全避免這個競態。

```c
int vfr_pool_dispatch_single(vfr_pool_t *pool, uint32_t slot_idx, vfr_frame_t *out_frame);
```
- **比喻**：「單一客戶模式——直接把車鑰匙連同停車位資訊交給他」。
- 單 Process / 單 Consumer 版本（Phase 1），不走 IPC，直接複製 metadata 到 `out_frame`。

#### 消費者端操作

```c
void vfr_pool_put_slot(vfr_pool_t *pool, vfr_frame_t *frame);
```
- **比喻**：「客戶還車鑰匙，管理員確認車位清空」（Phase 1 standalone 模式）。
- 原子遞減 `refcount`；降為 0 則呼叫 `platform->put_frame()` 釋放 DMA buffer。

#### Server 端回收操作

```c
void vfr_pool_server_release(vfr_pool_t *pool, uint32_t slot_id, uint64_t seq_num);
```
- **比喻**：「客戶寄回還車通知，管理員更新帳本」（Phase 2 IPC 模式）。
- Server 收到 Consumer 傳來的 `vfr_release_msg_t` 後呼叫。
- `seq_num = 0` 時跳過序號驗證（強制清理用）。

```c
void vfr_pool_force_release(vfr_pool_t *pool, uint32_t slot_id);
```
- **比喻**：「客戶死掉了，拖吊車直接把他的車位清空」（DROP\_OLDEST 策略）。
- 先設 `tombstone = true`，防止之後到來的遲延 release 訊息造成雙重釋放。
- 原子遞減 refcount；若降為 0 則立即清理。

#### 元資料查詢

```c
const vfr_frame_t *vfr_pool_slot_meta(vfr_pool_t *pool, uint32_t slot_idx);
int                vfr_pool_slot_dma_fd(vfr_pool_t *pool, uint32_t slot_idx);
```
- **比喻**：「查詢這格車位的停車資訊」/「拿到這格車位的鑰匙（原件）」。
- `slot_meta()` 給 Server 在 dispatch 時讀取 timestamp/width/height 等 metadata。
- `slot_dma_fd()` 給 Server 透過 `SCM_RIGHTS` 把鑰匙傳給 Consumer。

---

### 鈴鐺系統（`vfr_sync`）

**檔案：** `core/vfr_sync.h`、`core/vfr_sync.c`

> **停車場比喻**：這是停車場的**呼叫器系統**。
> Producer 停好新車後，不需要打電話輪詢——只要按一下呼叫器，Consumer 的 epoll 就立刻被喚醒。
> 採用 `EFD_SEMAPHORE` 模式：按 N 次鈴，就響 N 次，確保每一幀都被處理到。

```c
int vfr_sync_create_eventfd(void);
```
- **比喻**：「配發一個呼叫器給新入場的客戶」。
- 建立 `EFD_SEMAPHORE | EFD_CLOEXEC` 的 eventfd。

```c
void vfr_sync_close_eventfd(int *fd);
```
- 歸還呼叫器（關閉 fd），`*fd` 設為 -1，重複呼叫為 no-op。

```c
int vfr_sync_notify(int fd);
```
- **比喻**：「Producer 停好車後按下呼叫器（寫入 1）」。
- Consumer 的 `epoll_wait` 立刻收到 `EPOLLIN`，不耗任何 CPU 輪詢。

```c
int vfr_sync_drain(int fd);
```
- **比喻**：「Consumer 聽到鈴聲後，確認接收（讀出 1 次計數）」。
- `EFD_SEMAPHORE` 下每次 read 只減 1，多幀積壓時 `EPOLLIN` 會持續觸發。

```c
int vfr_sync_wait(int fd, int timeout_ms);
```
- **比喻**：「Consumer 坐在那裡等呼叫器響」。
- `timeout_ms < 0`：永久等待；`0`：立即回傳（非阻塞）。
- 回傳 `1` = 有事件；`0` = 逾時；`-1` = 錯誤。

---

## 層四：IPC 通訊協議

**檔案：** `ipc/vfr_ipc_types.h`

> **停車場比喻**：這是所有跨 Process 通訊用的**標準化表單**。
> 就像停車場有統一的「入場申請表」「發車通知單」「還車確認單」，
> 每張表都有防偽的暗語（magic number）確保不是亂填的表格。

### 握手流程（三次握手）

```
Client                          Server
  ──── vfr_client_hello_t ──────▶
       (入場申請表：PID、版本、策略)

  ◀─── vfr_handshake_t ──────────
       (成功確認：session_id、header_size)

  ◀─── vfr_shm_header_t ─────────
       (停車場平面圖：格式、尺寸、slot數)

  ◀─── vfr_eventfd_setup_t ──────  (含 SCM_RIGHTS：eventfd fd)
       (配發呼叫器)
```

### 各表單說明

```c
typedef struct { uint32_t magic; uint16_t proto_version;
                 int32_t consumer_pid; uint32_t policy; ... } vfr_client_hello_t;
```
- **比喻**：「入場申請表」。Consumer 在連線後送出的第一個訊息。
- `consumer_pid`：消費者的 PID，讓 Server 可以開 `pidfd` 監控其生死。
- `policy`：背壓策略，告訴 Server「滿了的時候我要怎樣被對待」。

```c
typedef struct { uint32_t magic; uint16_t proto_version;
                 uint32_t session_id; ... } vfr_handshake_t;
```
- **比喻**：「入場成功回執」。Server 分配的 `session_id` 是 Consumer 的「停車場會員號碼」，
  之後每一張還車通知單都要帶上這個號碼。

```c
typedef struct { uint32_t slot_id; uint32_t session_id;
                 uint32_t width; uint32_t height; uint64_t timestamp_ns; ... } vfr_frame_msg_t;
```
- **比喻**：「新車入場通知單」（配合 SCM\_RIGHTS 傳遞的 dma\_fd 就是「鑰匙」）。
- `slot_id`：哪格停車位（用於之後的還車通知）。

```c
typedef struct { uint32_t slot_id; uint32_t session_id;
                 uint64_t seq_num; ... } vfr_release_msg_t;
```
- **比喻**：「還車確認單」。Consumer 呼叫 `vfr_put_frame()` 後，Client 端自動發送此訊息。
- `seq_num`：防止「過期還車單」：如果 slot 已被重用，seq 不符就丟棄。

```c
typedef struct { uint32_t magic; uint32_t event_type;
                 float confidence; char stream_name[64]; ... } vfr_event_msg_t;
```
- **比喻**：「AI 部門送來的報案通知單」。AI process 偵測到事件後，
  透過 `\0/vfr/event/<stream_name>` 發送給 Recorder。

---

## 層五：服務端（Producer 側）

### 停車場管理總部（`vfr_server`）

**檔案：** `ipc/vfr_server.h`、`ipc/vfr_server.c`

> **停車場比喻**：這是整個停車場的**管理總部**。
> 它負責：蓋好停車場（init）、開門接客（accept）、分發鑰匙（produce）、
> 監控客戶死活（watchdog）、強制清場（teardown）。
> 整個 Server 是**單執行緒 event loop**（epoll 驅動），沒事就睡覺，有事立刻動。

#### 生命週期

```c
vfr_server_t *vfr_server_create(const char *stream_name, uint32_t slot_count);
```
- **比喻**：「蓋好停車場、拉好通訊管線、請好警衛（epoll）、準備好客戶清冊」。
- 內部流程：
  1. `vfr_pool_create()` — 劃出所有車位
  2. `socket() + bind()` — 在 Abstract Namespace 建立 Unix socket（`\0/vfr/<name>`）
  3. `epoll_create1()` — 啟動監控保全室
  4. 初始化所有 session 的 fd 為 `-1`（避免誤判 fd=0 為 stdin）

```c
void vfr_server_destroy(vfr_server_t **srv);
```
- **比喻**：「關門大吉，所有清理照原則五的順序做」。

#### 主迴圈（神經中樞）

```c
int vfr_server_handle_events(vfr_server_t *srv, int timeout_ms);
```
- **比喻**：「保全盯著監控牆，有事就處理」。
- `timeout_ms = 0`：非阻塞（立即回傳）；`-1`：無限等待。
- 內部處理三類事件：
  1. **新連線**（`listen_fd` EPOLLIN）→ `handle_accepted_client()`
  2. **客戶猝死**（`pidfd` EPOLLIN）→ `teardown_session()`（Phase 4 Watchdog）
  3. **客戶發訊息 / 斷線**（`client_fd` EPOLLIN/EPOLLHUP）→ `handle_release_msg()` / `teardown_session()`
- **穩定性細節**：同一批 epoll 事件中可能同時有「pidfd 死亡」和「socket 斷線」。
  處理前先用 `find_session_by_fd()` 確認 session 還活著，避免重複清理產生 `EBADF` 錯誤。

```c
int vfr_server_produce(vfr_server_t *srv);
```
- **比喻**：「工廠出貨——按合約決定怎麼把新車鑰匙送給每個客戶」。
- 內部流程（Phase A → B → C）：
  - **Phase A：決策**（依每個 Consumer 的 policy）：
    - `DROP_OLDEST`：呼叫 `force_release_slot()` 強制回收客戶最舊的幀，再給新幀
    - `BLOCK_PRODUCER`：呼叫 `wait_for_consumer_free()` 等客戶還車（最多 33ms）
    - `SKIP_SELF`：這幀不給這位客戶
  - **Phase B：原子鎖定**：`vfr_pool_begin_dispatch(n)` 一次設好 refcount
  - **Phase C：執行發送**：呼叫 `dispatch_to_session()`
    - `sendmsg` 傳 `vfr_frame_msg_t`，SCM\_RIGHTS 帶 dma\_fd
    - 按鈴：`vfr_sync_notify(eventfd)` 喚醒 Consumer
    - 若 sendmsg 失敗：立刻補償性呼叫 `vfr_pool_server_release()` 防止死鎖

#### 內部關鍵函式

```
handle_accepted_client()   — 辦理入場手續（握手 + eventfd 配發 + pidfd 開啟）
handle_release_msg()       — 處理客戶還車通知（正常流程）
force_release_slot()       — 強制拖吊（異常清理）
wait_for_consumer_free()   — 背壓等待（BLOCK_PRODUCER 策略）
dispatch_to_session()      — 宅配員（傳送 DMA-BUF fd + 按門鈴）
teardown_session()         — 完整清場（關閉所有 fd + 回收所有占用 slot）
```

#### 監控介面

```c
uint32_t vfr_server_get_drop_count(const vfr_server_t *srv);
uint32_t vfr_server_get_session_count(const vfr_server_t *srv);
```
- 供 Prometheus metrics 系統讀取目前的掉幀數與連線數（原子操作，隨時可呼叫）。

---

### 看板系統（`vfr_registry`）

**檔案：** `ipc/vfr_registry.h`、`ipc/vfr_registry.c`

> **停車場比喻**：這是停車場大門口的**電子看板（黃頁）**。
> Producer 登記：「我叫 `cam0`，在 `\0/vfr/cam0` 這裡，解析度 1920×1080」。
> Consumer 查詢：「現在有哪些影像流可以連？」
> 沒有看板，Consumer 就只能靠猜；有了看板，系統才能**動態擴充影像流**。

```c
int vfr_registry_serve_forever(void);
```
- **比喻**：「管理員坐在看板前值班，收發登記/查詢請求」。
- 啟動 Registry Daemon（阻塞），監聽 `\0/vfr/.registry`，收到 SIGTERM 後清理並退出。
- **必須最先啟動**：Producer/Consumer 比它早啟動時，連線會失敗但不影響核心功能（靜默 -1）。

```c
int vfr_registry_register(const vfr_stream_info_t *info);
```
- **比喻**：「Producer 到看板前登記自己」。
- 應在 `vfr_server_create()` 成功後呼叫。填入 stream\_name、解析度、PID 等資訊。

```c
int vfr_registry_unregister(const char *stream_name);
```
- **比喻**：「Producer 關閉時擦掉看板上的自己」。應在 `vfr_server_destroy()` 前呼叫。

```c
int vfr_registry_list(vfr_stream_info_t *entries, uint32_t max_count);
```
- **比喻**：「Consumer 看看看板上有什麼」。回傳目前在線的所有 stream 資訊。

---

### 心跳監測儀（`vfr_watchdog`）

**檔案：** `ipc/vfr_watchdog.h`、`ipc/vfr_watchdog.c`

> **停車場比喻**：這是停車場對每位客戶安裝的**心跳監測儀**。
> 只要客戶還活著（process 存在），監測儀安靜無聲。
> 一旦客戶猝死（Segfault / `kill -9`），監測儀立刻響警報——Server 的 epoll 收到通知，
> 立刻派清潔隊（`teardown_session`）回收所有被佔的車位。
> 這是 **Phase 4 的精髓**，比傳統 heartbeat 更精準，kernel 層面即時通知，零延遲。

```c
int  vfr_watchdog_open(pid_t pid);
```
- **比喻**：「幫這位客戶戴上心跳監測儀」。
- 呼叫 `pidfd_open(pid, 0)`（Linux 5.3+），取得一個監控指定 PID 的 fd。
- 將此 fd 加入 Server epoll（`EPOLLIN`），process 終止時 kernel 立刻通知。
- 回傳 `-1`（ENOSYS）：kernel 不支援 pidfd，系統退化為無 Watchdog 保護（仍可連線）。

```c
void vfr_watchdog_close(int *fd);
```
- 摘除心跳監測儀（客戶正常斷線時呼叫）。

```c
bool vfr_watchdog_available(void);
```
- 偵測此平台是否支援 `pidfd_open()`，握手時據此決定是否啟用 Watchdog。

---

## 層六：客戶端（Consumer 側）

**檔案：** `ipc/vfr_client.h`、`ipc/vfr_client.c`、`core/vfr_ctx.c`

> **停車場比喻**：這是客戶端的**全套辦理流程**——從找到停車場（registry）、
> 開口申辦（connect）、收到呼叫器（eventfd）、取車（recv\_frame）、還車（send\_release），
> 全部封裝在這裡。Consumer App 只需要呼叫公開 API（`vfr_open/get_frame/put_frame`）。

```c
int vfr_client_connect(const char *stream_name, vfr_client_state_t *out_state,
                       uint32_t policy);
```
- **比喻**：「辦理入場手續（完整三次握手）」。
- 連線到 `\0/vfr/<stream_name>`，發送 `vfr_client_hello_t`，
  接收 `vfr_handshake_t` + `vfr_shm_header_t` + eventfd（SCM\_RIGHTS）。
- 成功後 `out_state->eventfd` 就是這位客戶的呼叫器 fd。

```c
int vfr_client_recv_frame(vfr_client_state_t *state, vfr_frame_t *out_frame, int flags);
```
- **比喻**：「去窗口拿鑰匙與停車資訊」。
- 從 socket 接收 `vfr_frame_msg_t`（payload）+ dma\_fd（SCM\_RIGHTS）。
- 設定 `frame->priv = &state->slot_ref`（Back-reference，供 `put_frame` 回報用）。
- `VFR_FLAG_NONBLOCK`：沒資料時立即回傳（`MSG_DONTWAIT`）。

```c
int vfr_client_send_release(vfr_client_ref_t *ref);
```
- **比喻**：「寄出還車確認單」。
- 由 `vfr_put_frame()` 在偵測到 `frame->priv->mode == VFR_PRIV_CLIENT` 時自動呼叫。

```c
void vfr_client_disconnect(vfr_client_state_t *state);
```
- **比喻**：「退卡、關閉所有連線」。關閉 socket_fd 與 eventfd。

---

## 層七：錄影引擎（Recording Engine）

> **停車場比喻**：錄影引擎就像停車場旁邊的**安防監控中心**，
> 它有自己一套完整的「行車紀錄系統」：
> 持續錄下最近幾分鐘的影像到環形儲存（pre-roll），
> 一旦 AI 報警，立刻把報警前的片段 + 報警後的片段剪接存檔，
> 依排班表決定幾點到幾點才開錄。

---

### 錄影部門總管（`rec_engine`）

**檔案：** `rec/rec_engine.h`、`rec/rec_engine.c`

> **比喻**：所有錄影子模組的**總調度員**。
> 外界只需要和這位總管說話，不需要知道底下有哪些子部門。

```c
rec_engine_t *rec_engine_create(const rec_config_t *cfg);
```
- **比喻**：「成立錄影部門，按照企劃書（cfg）招募所有子部門成員」。
- 配置包含：stream\_name、錄影模式（CONTINUOUS/SCHEDULED/EVENT）、
  pre/post-roll 時長、分段大小、輸出目錄、Prometheus port 等。
- 內部初始化：`rec_buf`、`rec_writer`、`rec_state`、`rec_trigger`、
  `rec_schedule`、`rec_metrics`。

```c
int rec_engine_get_epoll_fds(rec_engine_t *eng, int *fds_out, int max_fds);
```
- **比喻**：「總管列出所有需要監控的警報線路（fd 清單）」。
- 呼叫者（main loop）把這些 fd 加入自己的 epoll。Engine 不持有 epoll，保持解耦。

```c
int rec_engine_handle_event(rec_engine_t *eng, int fd);
```
- **比喻**：「某條警報線路有動靜，總管判斷派哪個部門去處理」。
- 根據 fd 分派給：`rec_trigger`（AI 報警）、`rec_writer`（寫入完成事件）、
  `rec_metrics`（Prometheus scrape）。

```c
int rec_engine_push_frame(rec_engine_t *eng,
                           const uint8_t *data, uint32_t size,
                           uint64_t timestamp_ns, uint64_t seq_num,
                           bool is_keyframe, uint32_t format,
                           bool is_codec_config);
```
- **比喻**：「攝影師把拍到的影片片段遞給總管，總管決定怎麼處置」。
- `is_codec_config = true`（SPS/PPS/VPS）：發布到 codec config 快取，不計入影格。
- 一律推入 `rec_buf`（pre-roll 環形緩衝器）。
- 若當前狀態為 `IN_EVENT` 或 `WAIT_KEYFRAME`，也同步排入 `rec_writer` 寫入佇列。

```c
void rec_engine_destroy(rec_engine_t **eng);
```
- **比喻**：「錄影部門關門：先把最後的影片存好、再依序解散各子部門」。
- 先 flush `rec_writer`，再依序 destroy 各子模組，`*eng` 設為 NULL。

#### 狀態讀取

```c
rec_state_t rec_engine_get_state(const rec_engine_t *eng);
uint64_t    rec_engine_get_written_bytes(const rec_engine_t *eng);
uint32_t    rec_engine_get_dropped_frames(const rec_engine_t *eng);
```
- 供 metrics 系統讀取目前的錄影狀態、已寫入位元組、掉幀數。

---

### 值班狀態機（`rec_state`）

**檔案：** `rec/rec_state.h`、`rec/rec_state.c`

> **比喻**：就像安防控制中心的**值班警衛**，只有五種狀態，
> 嚴格按照規則切換，不會出現「一邊閒置一邊錄影」的混亂局面。

**五種狀態：**

```
IDLE ─── 收到 START 觸發 ───▶ WAIT_KEYFRAME ─── 等到 I-Frame ───▶ IN_EVENT
  ▲                                                                      │
  │                                                                      │
  └──────────── STOP 觸發 ◀── POST_WAIT ◀── STOP 觸發 ───────────────────┘
                               （倒數計時 post_sec 秒）
```

| 狀態 | 意義 | 停車場比喻 |
|---|---|---|
| `IDLE` | 閒置，只做 pre-roll 環形錄製 | 警衛坐在控制室，行車記錄器一直轉 |
| `WAIT_KEYFRAME` | 等待下一個 I-Frame 作為錄影起點 | 等一輛完整的車進來再開始錄 |
| `IN_EVENT` | 事件錄影中 | 主動錄影，把每幀寫入檔案 |
| `POST_WAIT` | 事件結束，錄完後尾（倒數計時） | 警報解除，再錄 30 秒確認安全 |
| `EXTRACT_PRE` | 保留（Prometheus gauge 相容性） | 預留狀態，目前同步完成 |

```c
void rec_state_init(rec_state_ctx_t *ctx, rec_buf_t *buf, uint32_t pre_sec);
```
- 初始化狀態機，關聯 ring buffer 與 pre-roll 時長。

```c
void rec_state_on_trigger(rec_state_ctx_t *ctx, rec_trigger_type_t type, uint64_t ts_ns);
```
- **比喻**：「接到 AI 報警電話（START）或解除電話（STOP）」。
- `IDLE + START`：呼叫 `rec_buf_extract_from_keyframe()` 提取 pre-roll，進入 `WAIT_KEYFRAME`。
- `POST_WAIT + START`：設 `pending_trigger = true`（等目前 POST 結束後立刻重啟）。

```c
void rec_state_on_timer_tick(rec_state_ctx_t *ctx, uint64_t expirations);
```
- **比喻**：「計時器報時（每秒一次）」。
- `POST_WAIT` 狀態中每秒遞減 `post_remaining_sec`；倒數為 0 → 回到 `IDLE`。

```c
void rec_state_on_keyframe(rec_state_ctx_t *ctx);
```
- **比喻**：「等到了一輛完整的車（I-Frame）」。
- `WAIT_KEYFRAME` → `IN_EVENT`：正式開始寫入。

```c
void rec_state_force_idle(rec_state_ctx_t *ctx);
```
- **緊急強制停止**：`rec_buf` 保護超時時呼叫，防止 Writer 卡死讓 ring buffer 動不了。

---

### 行車記錄器（`rec_buf`）

**檔案：** `rec/rec_buf.h`、`rec/rec_buf.c`

> **比喻**：這是一個**可覆蓋式行車記錄器（環形緩衝器）**。
> 它不斷錄下最近 N 秒的影像，像行車紀錄器一樣舊的自動被覆蓋。
> 當事件發生時，立刻從裡面找出事件前的片段（pre-roll）。

#### 結構設計

```
ring[]          ← 環形位元組緩衝區（15 MB 預設）
index[]         ← 各幀的 offset/size/timestamp 索引（最多 4096 幀）
pre_queue       ← 提取後要送給 Writer 的 pre-roll 幀佇列
protected_*     ← 保護讀取窗口（Writer 讀取中，ring 不可覆蓋此區域）
```

```c
rec_buf_t *rec_buf_create(uint32_t ring_size);
void       rec_buf_destroy(rec_buf_t **buf);
```
- 建立/銷毀環形緩衝器（預設 15 MB）。

```c
int rec_buf_push(rec_buf_t *buf, const uint8_t *data, uint32_t size,
                 uint64_t timestamp_ns, uint64_t seq_num, bool is_keyframe);
```
- **比喻**：「行車記錄器把新拍到的片段存入儲存媒介，舊的自動被覆蓋」。
- Ring 快要 wrap-around 時，自動驅逐最舊的 index entry。
- 若 Writer 的 `protected_read_offset` 被逼近（Writer 讀取太慢），
  等待最多 100ms（`REC_PROTECT_SPIN_TIMEOUT_US`）；超時則呼叫 `rec_state_force_idle()`。

```c
int rec_buf_extract_from_keyframe(rec_buf_t *buf, uint64_t now_ns,
                                   uint32_t pre_sec, uint32_t *batch_gen_out);
```
- **比喻**：「事件發生，從行車記錄器裡找出發生前 N 秒的片段，拷貝到輸出佇列」。
- 從 index 中掃描，找時間戳 ≤ `(now - pre_sec)` 且最接近的 I-Frame 作為起點。
- 若找不到就退而求其次用最舊的 I-Frame（確保一定有完整 GOP 起點）。
- 設定保護窗口（Writer 讀取期間 ring 不可覆蓋此區域），並遞增 `next_pre_gen`。

```c
void rec_buf_abort_pre(rec_buf_t *buf, uint32_t batch_gen);
```
- **比喻**：「取消剛才那次提取作業（例如 pre-roll 提取到一半被 STOP 了）」。
- 設 `aborted_pre_gen`，清除保護窗口。

```c
bool rec_pre_queue_dequeue(rec_pre_queue_t *q, rec_frame_entry_t *out);
```
- **比喻**：「Writer 從 pre-roll 佇列裡一筆一筆取出要寫入的幀」。

```c
void rec_buf_read_ring(const rec_buf_t *buf, uint32_t offset, uint32_t size, uint8_t *dst);
```
- **比喻**：「從行車記錄器的環形媒介中讀出指定片段（自動處理 wrap-around）」。

---

### 存檔工人（`rec_writer`）

**檔案：** `rec/rec_writer.h`、`rec/rec_writer.c`

> **比喻**：這是一個**獨立的存檔工人**（獨立 Thread）。
> 他有自己的工作佇列（`rec_write_queue`），從裡面一幀一幀取出資料，
> 封裝成 MPEG-TS 格式，寫進 `.ts` 檔案。
> 主控端（event loop）不會因為 I/O 慢而被拖住，工人在後台默默幹活。

```c
rec_writer_t *rec_writer_create(rec_buf_t *ring, rec_codec_config_t *codec_cfg,
                                 const rec_writer_config_t *cfg);
```
- **比喻**：「僱用存檔工人，給他需要的工具（ring buffer、codec config、輸出路徑）」。
- 內部啟動獨立 Thread，等待寫入佇列有資料或 shutdown sentinel。

```c
int rec_writer_get_eventfd(const rec_writer_t *w);
```
- **比喻**：「工人的通知鈴——工人處理完一批後用這個 fd 通知主控端」。
- 主控端把此 fd 加入 epoll（EPOLLIN），收到通知後呼叫 `rec_engine_handle_event()`。

```c
int rec_writer_enqueue(rec_writer_t *w, rec_write_item_t item);
```
- **比喻**：「把一幀資料放進工人的工作托盤」。
- `item.is_segment_boundary`：這幀是分段邊界，工人寫完後切換新檔案。
- `item.is_shutdown_sentinel`：關閉訊號，工人寫完後退出 Thread。
- 回傳 `REC_ERR_QUEUE_FULL`：工作托盤滿了（丟幀，遞增 `dropped_frames`）。

```c
void rec_writer_publish_codec_config(rec_writer_t *w, const uint8_t *data,
                                      uint32_t size, uint32_t format);
```
- **比喻**：「更新工人手上的影像解碼規格書（SPS/PPS）」。
- 使用 double-buffer（`slots[2]`）+ atomic swap，無鎖更新，工人隨時可讀到最新版。

```c
void rec_writer_destroy(rec_writer_t **w);
```
- 傳送 shutdown sentinel，等 Thread 結束，釋放資源。

```c
uint64_t rec_writer_get_written_bytes(const rec_writer_t *w);
uint32_t rec_writer_get_dropped_frames(const rec_writer_t *w);
```
- 原子讀取統計數據，供 Prometheus 回報。

---

### 捲宗分段（`rec_segment`）

**檔案：** `rec/rec_segment.h`、`rec/rec_segment.c`

> **比喻**：監控錄影不能一直錄成一個無限大的檔案，
> `rec_segment` 就像**捲宗管理員**，決定什麼時候該結束這份捲宗、開新的一份。
> 切換條件：超過 10 分鐘（預設）**或**超過 2GB。

```c
rec_segment_t *rec_segment_open(const char *output_dir, const char *stream_name,
                                 rec_mode_t mode, uint32_t max_duration_sec,
                                 uint64_t max_size_bytes, uint32_t flush_interval_sec);
```
- **比喻**：「開一份新捲宗（新的 .ts 檔案）」。
- 檔名格式：`<output_dir>/<stream_name>_<timestamp>_<mode>.ts`。
- `flush_interval_sec`：每 N 秒強制 `fsync()`，防止斷電遺失。

```c
int rec_segment_write(rec_segment_t *seg, const uint8_t *ts_buf, size_t size,
                      uint64_t duration_ns);
```
- **比喻**：「把一批 TS packets 寫進捲宗」。
- 內部追蹤已寫入大小與時長，超限時回傳信號讓 Writer 切換分段。

```c
void rec_segment_close(rec_segment_t *seg);
```
- **比喻**：「關閉這份捲宗（flush + close fd）」。

---

### MPEG-TS 封裝機（`rec_ts_mux`）

**檔案：** `rec/rec_ts_mux.h`、`rec/rec_ts_mux.c`

> **比喻**：這是**集裝箱打包機**。
> 原始的 H.264/H.265 資料是散裝貨物，MPEG-TS 是標準集裝箱格式。
> 打包好的集裝箱才能放進 .ts 檔案、被播放器解碼。

```c
int rec_ts_write_pat(uint8_t *buf, size_t buf_size, uint8_t *cc_pat);
```
- 寫入 PAT（Program Association Table）封包。每個 TS 段落的目錄。

```c
int rec_ts_write_pmt(uint8_t *buf, size_t buf_size, uint32_t format, uint8_t *cc_pmt);
```
- 寫入 PMT（Program Map Table）封包。說明「這個節目是 H.264 還是 H.265」。

```c
int rec_ts_write_pes(uint8_t *buf, size_t buf_size,
                      const uint8_t *payload, uint32_t payload_size,
                      uint64_t pts_ns, uint64_t pcr_ns,
                      bool is_keyframe, uint8_t *cc_video);
```
- **比喻**：「把一個 NAL unit 裝進集裝箱（PES + TS packets）」。
- `pts_ns` / `pcr_ns`：時間戳轉換為 TS 90kHz 時基。
- `is_keyframe`：I-Frame 時在 TS header 設 random\_access\_indicator。
- `cc_*`：continuity counter，4-bit 累計，由函式自動 mask+increment。
- 每個 TS 封包固定 188 bytes（`REC_TS_PACKET_SIZE`）。

---

### 班表系統（`rec_schedule`）

**檔案：** `rec/rec_schedule.h`、`rec/rec_schedule.c`

> **比喻**：就像保全公司的**排班表**——幾點到幾點，哪種模式值班（24/7 連續錄或只在特定時段錄）。

資料結構：一週 7 天 × 96 個時段（每 15 分鐘一格），每格 2 bits，共 24 bytes / 天。

```c
void rec_schedule_init_always(rec_schedule_t *sched, rec_slot_mode_t mode);
```
- **比喻**：「把所有班次填同一個模式」。例如全部設為 `REC_SLOT_CONTINUOUS`（24/7 不停錄）。

```c
void rec_schedule_set_slot(rec_schedule_t *sched, int wday, int slot_idx,
                            rec_slot_mode_t mode);
```
- **比喻**：「設定某天某個 15 分鐘時段的班次」。`slot_idx 0` = 00:00–00:15，`slot_idx 95` = 23:45–24:00。

```c
rec_slot_mode_t rec_schedule_query(const rec_schedule_t *sched, time_t now_wall_clock);
```
- **比喻**：「現在這個時間點，該怎麼錄？」
- 回傳 `REC_SLOT_OFF`、`REC_SLOT_CONTINUOUS`、`REC_SLOT_EVENT` 其中之一。

---

### AI 報警接收台（`rec_trigger`）

**檔案：** `rec/rec_trigger.h`、`rec/rec_trigger.c`

> **比喻**：這是停車場安防部門的**AI 報警電話接收台**。
> AI 模型（獨立 process）偵測到可疑人物時，發一個 UDP 風格的短訊（連線、送訊息、關閉）。
> 接收台接到後，立刻呼叫 `on_trigger` 通知錄影狀態機開始錄影。

```c
rec_trigger_t *rec_trigger_create(
    const char *stream_name,
    void (*on_trigger)(void *ud, rec_trigger_type_t type, uint64_t ts_ns),
    void *ud);
```
- **比喻**：「設立報警接收台，掛上緊急電話（abstract socket `\0/vfr/event/<stream_name>`）」。
- `on_trigger`：收到有效報警時的 callback，由錄影狀態機提供。

```c
int rec_trigger_get_fd(const rec_trigger_t *t);
```
- 取得 listen fd，加入 epoll（EPOLLIN）監控。

```c
void rec_trigger_handle_accept(rec_trigger_t *t);
```
- **比喻**：「電話來了，接起來、讀取報案內容、驗證格式、通知狀態機、掛掉」。
- 接受一個連線，讀 `sizeof(vfr_event_msg_t)`，驗證 magic（`0x45564E54` "EVNT"），
  呼叫 `on_trigger`，立刻 close connection（無長連線設計）。

```c
void rec_trigger_destroy(rec_trigger_t **t);
```
- 關閉 listen fd，釋放資源。

---

### 防誤報過濾器（`rec_debounce`）

**檔案：** `rec/rec_debounce.h`、`rec/rec_debounce.c`

> **比喻**：就像保全公司的**防誤報協定**。
> 如果報警鈴在 500ms 內響了兩次，第二次視為同一個事件的殘響，不重複處理。
> 防止 AI 模型因為連續幀輸出多次觸發，造成 Recorder 反覆重啟。

```c
void rec_debounce_init(rec_debounce_t *d, uint32_t debounce_ms);
```
- 初始化防抖動器，設定過濾窗口（預設 500ms = `REC_DEBOUNCE_MS`）。

```c
bool rec_debounce_filter(rec_debounce_t *d, rec_trigger_type_t type, uint64_t ts_ns);
```
- **比喻**：「這次報警是真的？還是上次的殘響？」
- `START`：若距上次 START < `debounce_ms`，視為重複，回傳 `false`（丟棄）。
- `STOP`：若距上次 START < `debounce_ms`，視為緊跟 START 後的假 STOP，也丟棄。
- 通過過濾的事件更新 `last_start_ns`。

---

### 即時報表系統（`rec_metrics`）

**檔案：** `rec/rec_metrics.h`、`rec/rec_metrics.c`

> **比喻**：錄影部門的**健康度報表**，用 Prometheus 格式輸出，讓監控平台（Grafana）能即時查看。

**輸出三個指標：**
- `rec_written_bytes_total`：已寫入的總 bytes（計數器）
- `rec_drop_frames_total`：掉幀數（計數器）
- `rec_state`：目前狀態機狀態（0=IDLE … 4=POST\_WAIT，gauge）

```c
rec_metrics_t *rec_metrics_create(const char *stream_name, uint16_t port);
```
- **比喻**：「開設報表閱覽室，掛上 TCP 牌號（port）」。`port = 0` 表示不開放（回傳 fd = -1）。

```c
void rec_metrics_set_providers(rec_metrics_t *m, void *ud,
    rec_state_t  (*get_state)(void *ud),
    uint64_t     (*get_written_bytes)(void *ud),
    uint32_t     (*get_dropped_frames)(void *ud));
```
- **比喻**：「讓報表系統知道要去哪裡問數據」。串接 callback 到錄影引擎的 accessor。

```c
int  rec_metrics_get_fd(const rec_metrics_t *m);
int  rec_metrics_serve_one(rec_metrics_t *m);
```
- **比喻**：「報表閱覽室開著，有人來了就給他看最新報表（HTTP/1.0 回應）」。
- 非阻塞：accept 一個連線，寫完就關，不 keep-alive。

```c
int rec_metrics_format(rec_metrics_t *m, char *buf, size_t buflen);
```
- 只格式化不發送，供測試驗證輸出格式。

---

## 層八：SDK 監控儀表板

**檔案：** `sdk/vfr_metrics.h`、`sdk/vfr_metrics.c`

> **比喻**：停車場（VFR Server）的**全自動儀表板**。
> 測量每幀的「到貨延遲（latency）」和「車位使用率（slot usage）」，
> 以 Prometheus text format 輸出，讓 Grafana 畫出漂亮的即時折線圖。

**輸出兩個 Histogram + 兩個 Gauge：**
- `vfr_frame_latency_seconds`：從 timestamp 到 dispatch 的延遲（7 個 bucket，1ms~100ms）
- `vfr_slot_usage_ratio`：Pool 使用率（5 個 bucket，0%~100%）
- `vfr_drop_frames_total`：掉幀數（從 server 讀取）
- `vfr_active_sessions`：目前連線中的 Consumer 數

```c
vfr_metrics_t *vfr_metrics_create(const char *stream_name, uint32_t slot_total);
```
- 建立 metrics handle，`slot_total` 用於計算 slot usage 比率。

```c
void vfr_metrics_set_server(vfr_metrics_t *m, vfr_server_t *srv);
```
- 串接 Server，metrics format 時自動讀取 `drop_count` / `session_count`。

```c
void vfr_metrics_observe_latency(vfr_metrics_t *m, uint64_t latency_ns);
void vfr_metrics_observe_slot_usage(vfr_metrics_t *m, uint32_t used);
```
- **比喻**：「每幀過磅（測延遲）並記錄車位使用情況」。
- thread-safe：使用原子累計，Producer 每幀呼叫，不影響效能。

```c
int  vfr_metrics_listen(uint16_t port);
int  vfr_metrics_serve_one(vfr_metrics_t *m, int listen_fd);
int  vfr_metrics_format(vfr_metrics_t *m, char *buf, size_t buflen);
void vfr_metrics_destroy(vfr_metrics_t **m);
```
- 與 `rec_metrics` 類似的 HTTP endpoint 邏輯。

---

## 完整生命週期流程

### 第零階段：系統啟動

```
① Registry Daemon 啟動
   vfr_registry_serve_forever()
   → 在 \0/vfr/.registry 掛上接收窗口，epoll_wait 開始值班

② VFR Server 啟動（相機驅動程式）
   vfr_server_create("cam0", 8)
   → 停車場蓋好、警衛室就位

③ 向 Registry 登記
   vfr_registry_register(&info)
   → 看板更新：「cam0 開放連線」
```

### 第一階段：Consumer 加入

```
④ Consumer 啟動（錄影 App / 預覽 App）
   vfr_open("cam0", 0) 或 vfr_client_connect("cam0", &state, policy)
   → 三次握手完成：
      Client → vfr_client_hello_t（版本 + PID + 策略）
      Server → vfr_handshake_t（session_id）
      Server → vfr_shm_header_t（格式協商）
      Server → vfr_eventfd_setup_t + SCM_RIGHTS（配發呼叫器）
   → Server 開啟 pidfd 監控 Consumer PID（心跳監測儀上線）
```

### 第二階段：影像分發循環（主迴圈）

```
⑤ Producer 有新幀
   vfr_server_produce()
   → vfr_pool_acquire()：找空車位，取幀
   → Phase A：對每個 Consumer 決策（DROP/BLOCK/SKIP）
   → Phase B：vfr_pool_begin_dispatch(n)：原子設定 refcount=n
   → Phase C：對每個有效 Consumer：
              dispatch_to_session()
              ├─ sendmsg(vfr_frame_msg_t + SCM_RIGHTS dma_fd)
              └─ vfr_sync_notify(eventfd)  ← 按門鈴

⑥ Consumer 收到通知（epoll 喚醒）
   vfr_get_frame() → vfr_client_recv_frame()
   ├─ recvmsg 接收 vfr_frame_msg_t + dma_fd
   └─ frame->dma_fd = 拿到的鑰匙

   ← Consumer 使用影像（mmap、硬體加速等）→

⑦ Consumer 用完影像
   vfr_put_frame(frame)
   → vfr_client_send_release()：寄出 vfr_release_msg_t
   → Server 收到 → vfr_pool_server_release()：refcount--
   → refcount = 0：platform->put_frame()，車位清空回 FREE
```

### 第三階段：異常處理

```
⑧ Consumer 突然崩潰（Segfault / kill -9）
   pidfd 的 EPOLLIN 觸發 → Server event loop 感知
   → teardown_session()：
      ├─ 對每個未歸還的 slot：force_release_slot()
      ├─ vfr_pool_force_release()：tombstone + refcount--
      ├─ close(eventfd)、close(pidfd)、close(socket_fd)
      └─ session 位置還給空閒池
   → 車位全部回收，停車場恢復正常
```

### 第四階段：錄影事件流

```
⑨ AI 偵測到事件
   AI process：connect \0/vfr/event/cam0 → send vfr_event_msg_t → close

⑩ rec_trigger_handle_accept()
   → 驗證 magic "EVNT"
   → rec_debounce_filter()：防誤報
   → rec_state_on_trigger(START)：
      ├─ rec_buf_extract_from_keyframe()：找 pre-roll 起點
      └─ 狀態機：IDLE → WAIT_KEYFRAME

⑪ 收到下一個 I-Frame
   rec_state_on_keyframe()：WAIT_KEYFRAME → IN_EVENT
   → 開始把每幀 enqueue 到 rec_write_queue

⑫ rec_writer（後台 Thread）
   ├─ 讀 pre_queue：把 pre-roll 幀讀出 + rec_ts_write_pes() 封裝
   ├─ 持續寫入 rec_segment（.ts 分段檔案）
   └─ 每 N 秒 fsync()

⑬ 事件結束
   rec_state_on_trigger(STOP)：IN_EVENT → POST_WAIT
   → 每秒 rec_state_on_timer_tick()：倒數 post_record_sec
   → 倒數歸零：POST_WAIT → IDLE
   → rec_writer 寫完最後一幀，rec_segment_close()
```

### 第五階段：關閉

```
⑭ VFR Server 關閉
   vfr_registry_unregister("cam0")  ← 擦掉看板
   vfr_server_destroy()
   → teardown 所有 active sessions
   → vfr_pool_destroy()
   → close listen_fd、epoll_fd

⑮ Recorder 關閉
   rec_engine_destroy()
   → 傳 shutdown sentinel 給 rec_writer
   → 等 Thread 結束（確保最後資料寫進去）
   → rec_segment_close()、rec_trigger_destroy()…
```

---

## 常數速查表

| 常數 | 值 | 位置 | 意義 |
|---|---|---|---|
| `VFR_DEFAULT_SLOTS` | 8 | `vfr_defs.h` | 預設 Buffer Pool 車位數 |
| `VFR_MAX_SLOTS` | 64 | `vfr_defs.h` | 最大車位數 |
| `VFR_MAX_CONSUMERS` | 16 | `vfr_defs.h` | 最多同時連線的 Consumer 數 |
| `VFR_MAX_CONSUMER_SLOTS` | 4 | `vfr_defs.h` | 每個 Consumer 最多持有的幀數 |
| `VFR_BLOCK_PRODUCER_TIMEOUT_MS` | 33ms | `vfr_defs.h` | BLOCK\_PRODUCER 最長等待（@30fps 一幀） |
| `VFR_WATCHDOG_TIMEOUT_MS` | 2000ms | `vfr_defs.h` | Watchdog 保底超時 |
| `VFR_REGISTRY_MAX_STREAMS` | 32 | `vfr_registry.h` | 看板最多容納的 stream 數 |
| `REC_BUF_SIZE_DEFAULT` | 15 MB | `rec_defs.h` | Pre-roll 環形緩衝器預設大小 |
| `REC_FRAME_INDEX_MAX` | 4096 | `rec_defs.h` | Index 最多記錄的幀數 |
| `REC_SEGMENT_DURATION_SEC` | 600s | `rec_defs.h` | 分段錄影預設時長（10 分鐘） |
| `REC_SEGMENT_SIZE_MAX` | 2 GB | `rec_defs.h` | 分段錄影最大檔案大小 |
| `REC_POST_RECORD_SEC_DEFAULT` | 30s | `rec_defs.h` | 事件後尾錄影預設時長 |
| `REC_DEBOUNCE_MS` | 500ms | `rec_defs.h` | 防誤報過濾窗口 |
| `REC_WRITE_QUEUE_DEPTH` | 256 | `rec_defs.h` | Writer 工作佇列深度 |
| `REC_PROTECT_SPIN_TIMEOUT_US` | 100ms | `rec_defs.h` | Ring 保護超時（Writer 卡住上限） |
| `REC_CODEC_CONFIG_MAX_SIZE` | 512 bytes | `rec_writer.h` | SPS/PPS 快取大小上限 |
| `REC_TS_PACKET_SIZE` | 188 bytes | `rec_ts_mux.h` | 標準 MPEG-TS 封包大小 |
| `REC_SCHEDULE_SLOTS_PER_DAY` | 96 | `rec_defs.h` | 每天 96 個 15 分鐘排班格 |

---

## 停車場概念在現實世界中的對應

這個「智慧停車場」系統裡幾乎每一個設計決策，都有對應的現實世界系統或主流程式設計模式。

### 1. Buffer Pool（停車位管理）→ 連線池 / 物件池

停車場預先劃好固定車位、循環使用，這就是**物件池（Object Pool）**模式。

| 停車場 | 現實對應 |
|---|---|
| 車位（Slot） | 資料庫連線池（PostgreSQL pgBouncer）的「連線」 |
| `acquire()` 取一個空位 | 從 thread pool 借一個 worker thread |
| `refcount` 歸零還回去 | C++ `shared_ptr` 的引用計數歸零自動釋放 |
| `VFR_MAX_SLOTS = 64` | nginx 的 `worker_connections` 上限 |

**核心思想**：「預先分配，反覆循環使用，避免頻繁 malloc/free 的開銷。」

---

### 2. Registry（看板）→ 服務發現

VFR Registry 的「登記 → 查表 → 連線」流程，和現代微服務架構一模一樣：

| 停車場 | 現實對應 |
|---|---|
| `vfr_registry_serve_forever()` | **Consul / etcd / ZooKeeper**（服務發現中心） |
| `vfr_registry_register()` | 微服務啟動時向 Consul 注册自己 |
| `vfr_registry_list()` | Kubernetes 的 `kubectl get services` |
| `vfr_registry_unregister()` | Pod 關閉時從 etcd 移除 endpoint |

DNS 也是同樣的邏輯——你不需要記 IP，問 DNS 就知道在哪。

---

### 3. 背壓策略（Backpressure）→ 流量控制

這是現代系統設計最重要的概念之一：

| 停車場策略 | 現實對應 |
|---|---|
| `DROP_OLDEST`（拖走最舊的車） | Kafka consumer 落後太多時，**earliest offset reset** |
| `BLOCK_PRODUCER`（叫生產者等） | TCP **滑動視窗（Flow Control）**：receiver buffer 滿了，傳 window=0 |
| `SKIP_SELF`（跳過這位客戶） | RxJava / Reactive Streams 的 `onBackpressureDrop()` |

Netflix 的 **Hystrix**、Go 的 **channel**（`chan` 滿了會阻塞 sender）都是背壓設計。

---

### 4. eventfd（呼叫器）→ 發布 / 訂閱

「生產者按鈴，消費者被動等待」就是 **Pub/Sub 模式**：

| 停車場 | 現實對應 |
|---|---|
| `vfr_sync_notify()` → `eventfd` | **Kafka** producer 寫入 topic |
| Consumer 的 `epoll_wait` 等呼叫器 | Kafka consumer 的 `poll()` |
| `EFD_SEMAPHORE`：N 次 write = N 次可 read | Redis `LPUSH` / `BLPOP` 計數語意 |
| 每個 Consumer 各有獨立 eventfd | Kafka 每個 consumer group 獨立 offset |

**Node.js 的 Event Loop** 也是一樣：用 `epoll`/`kqueue` 等待 IO 事件，不輪詢。

---

### 5. SCM\_RIGHTS 傳遞 FD（時空傳送盒）→ 零拷貝 + 能力型安全

把「鑰匙」傳給別人，而不是傳「車子本體」，這叫 **Zero-Copy** + **Capability-Based Security**：

| 停車場 | 現實對應 |
|---|---|
| 傳 `dma_fd`，影像不動 | Linux `sendfile()`：檔案 bytes 不過 userspace |
| SCM\_RIGHTS 跨 process 傳 fd | Android Binder 傳 `FileDescriptor` 給其他 App |
| Consumer 拿到 fd 才能讀影像 | macOS Sandbox：你拿到 file descriptor 才有存取權 |

**DPDK / io\_uring** 的零拷貝網路也是同樣思想——數據不搬，傳「指向它的憑証」。

---

### 6. pidfd / Watchdog（心跳監測）→ 健康檢查

「客戶猝死，立刻清理」的機制在現實中無處不在：

| 停車場 | 現實對應 |
|---|---|
| `pidfd_open()` 監控 Consumer PID | **Kubernetes liveness probe**（container 死了重啟） |
| `teardown_session()` 自動清理 | **systemd** 監控 service，死了自動 `Restart=always` |
| Watchdog 不需輪詢，kernel 即時通知 | **AWS ELB Health Check**（偵測到 instance 掛掉，從 target group 移除） |

---

### 7. 環形緩衝器（行車記錄器）→ 到處都是

`rec_buf` 的「新資料蓋掉最舊的」模式是嵌入式與系統程式中最古老的資料結構之一：

| 停車場 | 現實對應 |
|---|---|
| pre-roll ring buffer | 監控攝影機的**行車記錄器**（覆蓋式 SD 卡錄影） |
| `rec_buf_extract_from_keyframe()` | 飛機**黑盒子**：事故後取出事發前 N 分鐘 |
| Linux kernel `kfifo` | 網路 driver 的 packet ring buffer（`AF_PACKET` TPACKET） |
| 音訊 DAW 的 buffer | Pro Tools / GarageBand 的 ring buffer 避免爆音 |

---

### 8. 狀態機（值班警衛）→ 幾乎所有業務邏輯

`rec_state` 的五個狀態轉換，和以下系統同構：

| 停車場狀態 | 現實對應 |
|---|---|
| `IDLE → WAIT_KEYFRAME` | 紅綠燈：紅燈 → 等綠燈（等 I-Frame = 等可以通行的起點） |
| `IN_EVENT → POST_WAIT` | 電梯門：有人進出 → 門等 N 秒後才關 |
| Redux / XState | 前端狀態管理：`IDLE / LOADING / SUCCESS / ERROR` |
| TCP 狀態機 | `LISTEN → SYN_RCVD → ESTABLISHED → FIN_WAIT → CLOSED` |

---

### 小結：永恆的計算機科學原語

整個 VFR 系統其實是把以下幾個**永恆的計算機科學原語**組合在一起：

```
物件池 + 服務發現 + 背壓 + Pub-Sub + 零拷貝 + 健康檢查 + 環形緩衝 + 狀態機
```

這些概念從 1970 年代的 Unix 一直延續到今天的 Kubernetes、Kafka、Rust async，
**本質從未改變**，只是換了包裝。這也是為什麼學好底層 Linux 系統程式設計，
看現代雲端架構時會覺得「這不就是 XXX 嘛」。

---

*文件自動從源碼標頭與 docs/ 整合產生。如有架構變更，請同步更新本文件。*
*Last updated: 2026-04-30*
