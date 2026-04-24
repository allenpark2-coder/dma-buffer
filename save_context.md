# VFR 專案 Context 備份 — 接續 Phase 5

## 專案位置
`/home/allen/vivotek/dma-buffer`

## 參考文件（必讀）
- `plan-vfr-cross-platform-v2.md` — 完整架構計畫（v2.4）
- `POOL_DESIGN.md` — Slot 狀態機、FD 生命週期、Dispatch 機制（v1.1）

---

## 目前完成狀態

### Phase 1 ✅ — 單 Process 影像讀取
- `include/vfr_defs.h` — 唯一常數定義點
- `include/vfr.h` — 消費者 API
- `core/vfr_pool.c/h` — Slot Allocator（狀態機 + refcount）
- `core/vfr_ctx.c` — vfr_open/close/get_frame/put_frame/get_eventfd
- `sdk/vfr_map.c` — mmap + DMA_BUF_IOCTL_SYNC
- `platform/mock/mock_adapter.c` — memfd mock（has_native_dma_buf=false）
- `platform/amba/amba_adapter.c` — Amba IAV5/6 skeleton
- `test/test_single_proc.c`

### Phase 2 ✅ — IPC 跨 Process FD 傳遞
- `ipc/vfr_ipc_types.h` — 所有 IPC 訊息型別
- `ipc/vfr_server.c/h` — SCM_RIGHTS Server
- `ipc/vfr_client.c/h` — FD Receiver
- `test/test_ipc_producer.c` + `test/test_ipc_consumer.c`

### Phase 3 ✅ — 低延遲多端消費 + Backpressure
- `core/vfr_sync.c/h` — EventFD + Epoll helper
- Backpressure Policy（DROP_OLDEST / BLOCK_PRODUCER / SKIP_SELF）
- 每 consumer 獨立 eventfd（producer dispatch 後各寫一次）
- `test/test_multicast.c` — 3 consumer（RTSP + Recorder + Motion）

### Phase 4 ✅ — 容錯與資源回收（剛完成）
- `ipc/vfr_watchdog.c/h` — `pidfd_open()` wrapper
  - `vfr_watchdog_open(pid)` → pidfd；`vfr_watchdog_close(fd*)`
  - `vfr_watchdog_available()` → bool
  - Fallback：kernel < 5.3 回傳 -1，仍靠 socket HUP
- `ipc/vfr_server.c` 修改：
  - `consumer_session_t` 新增 `pidfd`、`pid` 欄位
  - `handle_accepted_client()`：握手後 `vfr_watchdog_open()` + 加入 epoll
  - `teardown_session()`：tombstone(active=false) → epoll DEL(socket+pidfd) → close fd → close pidfd → 歸還 slot
  - `vfr_server_handle_events()`：pidfd EPOLLIN → teardown；stale fd 靜默忽略
  - `vfr_server_get_drop_count()` / `vfr_server_get_session_count()` 監控介面
- `test/test_crash_recovery.c` — fork-based self-test（`--self-test` / `--role producer|crasher`）
- Makefile：新增 `test_crash_recovery`、`check4`、`check4-ext`、`asan4`

---

## 目錄結構（現況）

```
vfr/
├── include/
│   ├── vfr_defs.h
│   └── vfr.h
├── core/
│   ├── vfr_ctx.c
│   ├── vfr_pool.c / vfr_pool.h
│   └── vfr_sync.c / vfr_sync.h
├── ipc/
│   ├── vfr_ipc_types.h
│   ├── vfr_server.c / vfr_server.h
│   ├── vfr_client.c / vfr_client.h
│   └── vfr_watchdog.c / vfr_watchdog.h   ← Phase 4 新增
├── platform/
│   ├── platform_adapter.h
│   ├── mock/mock_adapter.c
│   └── amba/amba_adapter.c
├── sdk/
│   └── vfr_map.c
│   (bridge_opencv.c / bridge_gstreamer.c → Phase 5 待建)
├── test/
│   ├── test_single_proc.c
│   ├── test_ipc_producer.c / test_ipc_consumer.c
│   ├── test_multicast.c
│   └── test_crash_recovery.c              ← Phase 4 新增
└── Makefile
```

---

## Phase 5 任務（按優先度）

計畫原文（`plan-vfr-cross-platform-v2.md` Phase 5 節）：

> **目標**：提供便利性工具，讓 AI / 串流模組快速接入。

| 優先度 | 項目 |
|--------|------|
| 高 | OpenCV Bridge：`vfr_to_mat(frame, mat)` 直接構造 `cv::Mat`（零複製） |
| 中 | GStreamer Bridge：`vfrsrc` element，讓 frame 進入 GStreamer pipeline |
| 中 | Prometheus metrics endpoint：`drop_count`、latency histogram、**slot usage histogram** |
| 低 | Registry Table：動態 stream 發現（仿 ZeroMQ ServiceBinder） |

**log 等級提醒**：Phase 5 metrics 輸出應受 `VFR_LOG_LEVEL` 控制，
`INFO` 等級只輸出彙整統計，`DEBUG` 等級才輸出每幀數值。

---

## 關鍵設計決策（不得自行更改）

| 項目 | 決策 | 位置 |
|------|------|------|
| 通用貨幣 | DMA-BUF fd（memfd 降級） | `vfr_frame_t.dma_fd` |
| IPC 通道 | Unix Abstract Namespace `\0/vfr/<stream>` | `vfr_server.c` |
| 消費者通知 | EventFD（每 consumer 獨立） | `vfr_sync.c` |
| Slot 動態調整 | 不實作 | `vfr_defs.h: VFR_MAX_SLOTS=64` |
| 容錯偵測 | `pidfd_open()` + epoll | `vfr_watchdog.c` |
| Signal Handler | `sigaction()`，禁止 `signal()` | 各 main() |
| 亂數 | `getrandom()`，禁止 `rand()` | 未來需要時 |
| Zero-copy mapper | `vfr_map.c`：mmap → SYNC_START → read，SYNC_END → munmap | `sdk/vfr_map.c` |

---

## 重要常數（`include/vfr_defs.h`）

```c
#define VFR_SHM_MAGIC           0x56465231u   // "VFR1"
#define VFR_PROTO_VERSION       2u
#define VFR_DEFAULT_SLOTS       8
#define VFR_MAX_SLOTS           64
#define VFR_MAX_CONSUMERS       16
#define VFR_MAX_CONSUMER_SLOTS  4
#define VFR_MAX_PLANES          3
#define VFR_SOCKET_NAME_MAX     64
#define VFR_WATCHDOG_TIMEOUT_MS     2000
#define VFR_BLOCK_PRODUCER_TIMEOUT_MS 33
```

---

## Makefile 現有 targets

| Target | 說明 |
|--------|------|
| `make all` | 編譯所有 binary |
| `make check` | Phase 1 驗收（需 valgrind） |
| `make check2` | Phase 2 IPC 驗收 |
| `make check3` | Phase 3 Multicast 驗收 |
| `make check4` | Phase 4 crash recovery self-test |
| `make check4-ext` | Phase 4 外部 kill -9 測試 |
| `make asan3` | Phase 3 ASan build |
| `make asan4` | Phase 4 ASan build |

---

## 環境設定

| 環境變數 | 值 | 說明 |
|---------|-----|------|
| `VFR_MODE` | `client` / `standalone`(預設) | vfr_open 模式 |
| `VFR_PLATFORM` | `mock`(預設) / `amba` | Platform adapter |
| `VFR_POLICY` | `drop_oldest` / `block_producer` / `skip_self` | Consumer backpressure |
| `VFR_MOCK_BINARY` | `<path>` | 播放預錄 YUV |
| `DEBUG=1` | — | 啟用 debug log（VFR_LOG_LEVEL=3） |

---

## Build 系統
- `gcc -Wall -Wextra -std=c11 -D_GNU_SOURCE`
- `-I. -Iinclude -Icore`
- 依賴：Linux 5.4+、glibc 2.27+（memfd_create、pidfd_open、getrandom）

## git log（最近）
```
Phase 4: Robust — 容錯與資源回收（vfr_watchdog + teardown 重構）
Phase 3: Sync — 低延遲多端消費與 Backpressure Policy
Phase 2: IPC cross-process DMA-BUF fd passing via SCM_RIGHTS
Initial commit for dma-buffer
```
