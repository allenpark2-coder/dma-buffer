# VFR 系統引導式學習筆記
> 用「智慧停車場」比喻，從問題出發理解每個設計決策

---

## 學習地圖

```
站 1 → Buffer Pool（停車場本體）
站 2 → Backpressure 背壓（車位滿了怎辦）
站 3 → Registry（怎麼找到停車場）
站 4 → eventfd / Sync（怎麼通知 Consumer 有新幀）
站 5 → SCM_RIGHTS / 零拷貝（怎麼傳鑰匙）
站 6 → Watchdog / pidfd（Consumer 猝死怎辦）
```

---

## 站 1：Buffer Pool（停車場）

### 問題：每次 malloc/free 的代價

最直覺的做法：

```c
uint8_t *buf = malloc(4 * 1024 * 1024);  // 每幀 4MB
// 填入影像...
send_to_app(buf);
free(buf);
```

**兩個致命問題：**

**問題一：每次都花時間 malloc/free**
- `malloc` 需要找空閒記憶體、更新 heap metadata，可能要跟 kernel 要更多記憶體
- 每秒 30 次、每次 4MB → 顯著 CPU 浪費，而且時間不穩定

**問題二：記憶體碎片化**

```
時間 0: [  空  ][  空  ][  空  ][  空  ]
時間 1: [幀 A ][幀 B ][幀 C ][  空  ]   ← malloc x3
時間 2: [  空  ][幀 B ][  空  ][  空  ]  ← free A, C
時間 3: 要 malloc 8MB → 失敗！（有空間但不連續）
```

---

### 解法：Buffer Pool — 預先劃好車位，循環使用

```
啟動時一次蓋好停車場：
[ 車位 0 ][ 車位 1 ][ 車位 2 ][ 車位 3 ]...
  FREE      FREE      FREE      FREE

幀來了：不 malloc，直接找一個 FREE 的車位
幀用完：不 free，標回 FREE，下次再用
```

每個「車位」叫做 **Slot**，有三種狀態：

```c
typedef enum {
    SLOT_FREE,       // 空車位，可以停新車
    SLOT_FILLING,    // 正在停車（Platform 正在填影像）
    SLOT_IN_FLIGHT,  // 鑰匙已發出去，有人在用
} slot_state_t;
```

**Slot 生命週期：**

```
FREE
 │  vfr_pool_acquire()
 ▼
FILLING
 │  vfr_pool_begin_dispatch() + dispatch
 ▼
IN_FLIGHT
 │  Consumer 用完，refcount 歸零
 ▼
FREE  ← 回到起點
```

**有了 Pool 之後：**

| | 每次 malloc/free | Buffer Pool |
|---|---|---|
| 速度 | 慢且不穩定 | O(1)，固定快 |
| 記憶體碎片 | 會累積 | 完全沒有 |
| 記憶體上限 | 不可控 | 固定（VFR_MAX_SLOTS = 64） |

---

### 關鍵細節：refcount 與 begin_dispatch(n)

同時有 3 個 Consumer 時，同一幀要發 3 份鑰匙：

```c
vfr_pool_begin_dispatch(pool, slot_idx, 3);  // refcount = 3
// 發給 A → B → C
// A 還：refcount = 2
// C 還：refcount = 1
// B 還：refcount = 0 → 車位自動 FREE
```

**為什麼要先設好 n，再發鑰匙？**

「先發後加」的競態問題：

```
T1: 發給 A，refcount = 1
T2: A 超快還車，refcount = 0 → 系統清空車位！
T3: 才要發給 B → B 拿到指向已釋放記憶體的鑰匙 💥
```

「先設 n=3 再發」的正確方式：

```
設 refcount=3 → 發給 A → 發給 B → 發給 C
refcount 在三個人都還完之前永遠不會歸零
```

**同一幀能不能中途加 Consumer？**

不行。每幀的 refcount 一旦設定就固定。新 Consumer 連線後從**下一幀**開始收，不會插進正在 dispatch 的幀。每幀的 n 是在 dispatch 瞬間對當下在線人數取快照。

---

## 站 2：背壓（Backpressure）

### 問題：車位全滿，新車停不進來

8 個車位全滿，第 9 台車來了。管理員有三種選擇：

**選擇 A：DROP_OLDEST（強制拖吊）**

```
找出 Consumer C 手上最舊的 slot
→ tombstone = true（standalone 模式）
→ refcount--
→ refcount == 0 → slot 回 FREE
→ 新幀停進來 ✓
```

**選擇 B：BLOCK_PRODUCER（叫新車等）**

```
while (沒有空車位) {
    sleep(1ms)
    if (等超過 33ms) { 放棄這幀; break; }
}
```
33ms = 1000ms ÷ 30fps，等超過一幀就沒意義

**選擇 C：SKIP_SELF（跳過這位客戶）**

```
begin_dispatch(n=2)  ← 只算 A 和 B，跳過 C
C 這幀沒收到，但手上現有的幀完全安全
C 下一幀如果還完了舊的，正常繼續收
```

---

### 三種策略適用場景

| Consumer 類型 | 策略 | 原因 |
|---|---|---|
| RTSP 串流（要即時） | DROP_OLDEST | 要最新畫面，掉幾幀沒差 |
| 錄影引擎（不能掉幀） | BLOCK_PRODUCER | 寧願讓相機等，不能有缺口 |
| AI 推論（偶爾跳幀沒差） | SKIP_SELF | 跳幀不影響分析，且不傷其他人 |

三個 Consumer 共用同一個 Buffer Pool，各自用不同策略，互不干擾。

**三個策略的本質：**

```
DROP_OLDEST    → 犧牲 Consumer 的舊幀  （即時性優先）
BLOCK_PRODUCER → 犧牲 Producer 的時間  （完整性優先）
SKIP_SELF      → 犧牲 Consumer 的新幀  （獨立性優先）
```

---

## 站 3：Registry（停車場入口看板）

### 問題：Consumer 怎麼知道停車場在哪？

寫死地址的三個問題：
1. 有哪些 stream？Consumer 怎麼知道？
2. stream 改名了，所有 Consumer 都要重編？
3. Consumer 比 Server 早啟動，要輪詢嗎？

---

### 解法：加一個固定地址的「中間人」

```
Registry 永遠在：\0/vfr/.registry   ← 這個地址永不變

Producer 啟動：「我叫 cam0，在 \0/vfr/cam0，1920x1080」
Consumer 查詢：「現在有哪些停車場？」
看板回答：    「cam0、cam0_sub，都在線」
```

**三方互動：**

```
① Registry Daemon 最先啟動，掛在固定地址等待

② Producer 啟動後登記自己
   vfr_registry_register(&info)

③ Consumer 啟動時先查看板
   vfr_registry_list(entries, max)
   → 得知有哪些 stream → 再連線
```

**Registry 解決 / 沒解決的事：**

```
✅ 解決：地址在哪（動態查，不用寫死）
✅ 解決：現在有哪些 stream 在線
✅ 解決：Consumer 可以寫成通用型
✅ 解決：Server 還沒起來時 Consumer 如何等待

❌ 沒解決：新 stream 出現，業務邏輯怎麼處理
          （這是應用程式的責任，不是基礎設施的責任）
```

**Registry 掛掉怎辦？**

靜默失敗，不影響核心功能。Producer/Consumer 還是可以用固定地址直連，只是少了動態查詢。

> 看板壞了，但你還是知道停車場在哪，還是可以停車。

**核心原則：基礎設施（Infrastructure）和業務邏輯（Business Logic）是兩層，要分開。**

---

## 站 4：eventfd / Sync（怎麼通知 Consumer）

### 問題：輪詢的代價

```c
while (true) {
    int ret = vfr_get_frame(ctx, &frame, VFR_FLAG_NONBLOCK);
    if (ret == 0) { process(frame); }
    // 沒有新幀：繼續問（CPU 空轉）
}
```

```
30fps = 每幀 33ms
輪詢間隔 1ms → 33 次裡只有 1 次有用，32 次空轉
CPU 使用率：97% 在做無用功

16 個 Consumer 同時輪詢 → 系統整體效能崩潰
```

---

### 解法：eventfd — 等門鈴，不輪詢

```
輪詢 = 每分鐘打電話問「到了嗎？到了嗎？」
eventfd = 外送到了按門鈴，你才開門
```

```c
// Server：停好新幀，按門鈴
vfr_sync_notify(consumer->eventfd);   // write(fd, 1)

// Consumer：睡覺等門鈴（CPU 完全不佔用）
vfr_sync_wait(eventfd, timeout_ms);   // epoll_wait
```

**完整流程：**

```
Consumer 把 eventfd 加入 epoll，然後睡覺（zzz...）
         ↓
Producer 停好新幀：vfr_sync_notify() 寫入 1
         ↓
kernel 喚醒 epoll_wait
         ↓
Consumer 醒來：vfr_get_frame() 拿幀
```

**EFD_SEMAPHORE：連續多幀不漏**

```
普通 eventfd：Producer 連按 3 次 → Consumer 只醒 1 次（值清零）
EFD_SEMAPHORE：Producer 連按 3 次 → Consumer 醒 3 次（每次 read 只減 1）
```

**每個 Consumer 有獨立的 eventfd：**

```
Consumer A: fd=7
Consumer B: fd=8
Consumer C: fd=9

Producer 按鈴時，三個各自響
A 慢吞吞沒醒 → 不影響 B 和 C
```

| | 輪詢 | eventfd |
|---|---|---|
| 沒有新幀時 | CPU 空轉 | CPU 讓給別人 |
| 16 個 Consumer | 16 個 thread 空轉 | 全部睡覺 |
| 喚醒延遲 | 取決於輪詢間隔 | kernel 即時，微秒級 |
| 幀會不會漏 | 可能漏 | EFD_SEMAPHORE 保證不漏 |

---

## 站 5：SCM_RIGHTS（零拷貝傳影像）

### 問題：複製資料的代價

```
每幀 4MB × 30fps = 120MB/s
4 個 Consumer：480MB/s 記憶體複製
4 路相機：1.92GB/s → 嵌入式 CPU 全部拿去複製，什麼都做不了
```

---

### 先理解：fd 是「鑰匙」不是「車子」

```c
int fd = open("photo.jpg", O_RDONLY);
// fd 是整數（如 5），不是檔案內容
// fd=5 → kernel file 物件 → 硬碟資料（資料本身沒動）
```

DMA buffer 也一樣：

```
fd=7 → kernel dma_buf 物件 → 硬體記憶體（影像資料）
```

**問題：fd 是 Process 私有的**

```
Producer: fd=7 → DMA buffer A
Consumer: fd=7 → 完全不同的東西！
```

不能直接傳數字，每個 Process 的 fd 表不同。

---

### 解法：SCM_RIGHTS — 讓 kernel 幫你複製鑰匙

```c
// Producer：sendmsg 夾帶 fd
sendmsg(socket_fd, &msg_with_SCM_RIGHTS, 0);

// Consumer：recvmsg 接收
recvmsg(socket_fd, &msg, 0);
// 得到 fd=12（新號碼，但和 Producer 的 fd=7 指向同一塊硬體記憶體）
```

```
傳送後：
Producer: fd=7  ──▶ kernel dma_buf ──▶ 硬體記憶體
Consumer: fd=12 ─────────────────────────↑
                      同一塊！資料沒有移動過！
```

**完整流程：**

```
① Producer 停好新幀，slot 裡有 dma_fd=7
② sendmsg：傳 vfr_frame_msg_t（metadata，約 64 bytes）
           SCM_RIGHTS 夾帶 dma_fd=7
③ Consumer recvmsg：收到 metadata + dma_fd=12
④ Consumer mmap(dma_fd=12) → 直接讀硬體記憶體（零拷貝）
⑤ Consumer 用完：
   vfr_put_frame()  → 通知 Server 帳本 refcount--
   close(dma_fd=12) → 通知 kernel 硬體引用計數--
```

**兩個層面的「還鑰匙」：**

| 動作 | 對象 | 意義 |
|---|---|---|
| `vfr_put_frame()` | Server（帳本） | slot refcount--，讓 slot 可以回 FREE |
| `close(dma_fd)` | kernel | dma_buf 引用計數--，讓硬體記憶體可以釋放 |

兩件事都要做，缺一不可：
- 只還帳本不 close fd → kernel 引用計數不歸零，硬體記憶體永遠不釋放
- 只 close fd 不還帳本 → slot refcount 永遠不歸零，停車場慢慢被塞滿 💥

| | 複製 | SCM_RIGHTS |
|---|---|---|
| 每幀傳輸量 | 4MB | ~64 bytes |
| 4 個 Consumer | 16MB/幀 | ~256 bytes/幀 |
| CPU 記憶體頻寬 | 被吃光 | 幾乎為零 |

---

## 站 6：Watchdog / pidfd（Consumer 猝死怎辦）

### 問題：Consumer 突然 Segfault

```
Consumer 手上持有 slot 2、3、4
突然崩潰 → 什麼清理都來不及執行

slot 2、3、4 狀態：永久 IN_FLIGHT
refcount：永遠不歸零
停車場 8 個車位，3 個永久鎖死
```

---

### 偵測機制比較

**心跳（Heartbeat）：**
- Consumer 每秒送「我還活著」訊息
- 延遲：秒級，需要 Consumer 實作額外邏輯

**Socket 斷線（EPOLLHUP）：**
- Consumer 死 → socket 自動關閉 → epoll 收到通知
- 延遲：毫秒級，但有時序問題

**pidfd（最佳方案）：**

```c
// 握手時 Consumer 告訴 Server 自己的 PID
int pidfd = pidfd_open(consumer_pid, 0);
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pidfd, &event);

// Consumer 死掉的瞬間：
// kernel 立刻把 pidfd 標記為 readable
// epoll_wait 馬上醒來 → teardown_session()
```

| | 心跳 | Socket 斷線 | pidfd |
|---|---|---|---|
| 偵測延遲 | 秒級 | 毫秒級 | 微秒級 |
| Consumer 需額外實作 | ✅ | ❌ | ❌ |
| 可靠性 | 不穩定 | 有時序問題 | kernel 保證 |

---

### teardown_session() 清理流程

```
找出 Consumer 手上所有 slot（slot 2、3、4）
↓
對每個 slot 執行 force_release：
  refcount--
  refcount == 0 → slot 回 FREE ✓
↓
關閉所有 fd：
  close(eventfd)
  close(pidfd)
  close(socket_fd)
↓
session 位置還回空閒池
→ 整個過程：微秒級完成
```

---

### IPC 模式的遲來訊息防護：refslot[]

**場景：Consumer 在死前剛好送出了 release 訊息**

```
T=1ms：Consumer 送出 release(slot_2)，訊息在 socket buffer 排隊
T=2ms：Consumer 崩潰
T=3ms：Server 偵測到死亡，執行 teardown：
        ① 先從 refslot[] 移除 slot_2（標記「A 不再擁有此 slot」）
        ② force_release(slot_2)：refcount--（不設 tombstone）
T=5ms：Server 讀到 T=1ms 的遲來 release 訊息
        → handle_release_msg()
        → 查 A 的 refslot[]：slot_2 已被移除！
        → 「這不是 A 的 slot」→ 忽略 ✓
```

**refcount=2，一個 Consumer 死掉的情況：**

```
Consumer A 死，Consumer B 還活著
→ teardown 移除 A 的 refslot[slot_2]
→ force_release(slot_2)：refcount 2→1（不設 tombstone）
→ slot_2 繼續 IN_FLIGHT
→ Consumer B 正常使用，正常還車
→ server_release(slot_2)：refcount 1→0 → FREE ✓
```

---

### tombstone 的真正用途

tombstone 是給 **standalone 單 Process 模式**用的，不是 IPC 模式的主要防線：

| 模式 | 防重複釋放的機制 |
|---|---|
| IPC 多 Process | `refslot[]` 移除 → 遲來訊息被過濾 |
| Standalone 單 Process | `tombstone = true` → 遲來的 put_slot 被攔截 |

程式碼注釋原文：
```c
/* IPC 模式下不設 tombstone
 * 由呼叫方先從 refslot[] 移除，
 * handle_release_msg 不會再次觸發 server_release */
```

---

## 六站完整結論

```
站 1 Buffer Pool   → 解決：記憶體碎片 + malloc/free 效能問題
站 2 Backpressure  → 解決：資源滿了，不同 Consumer 用不同策略
站 3 Registry      → 解決：Consumer 動態發現 Producer 的位置
站 4 eventfd       → 解決：有新幀時通知 Consumer，不浪費 CPU
站 5 SCM_RIGHTS    → 解決：跨 Process 傳影像，完全零拷貝
站 6 Watchdog      → 解決：Consumer 猝死，資源自動回收
```

**永恆的計算機科學原語：**

```
物件池 + 服務發現 + 背壓 + Pub-Sub + 零拷貝 + 健康檢查
```

這些概念從 1970 年代的 Unix 到今天的 Kubernetes、Kafka、Rust async，本質從未改變。

---

**現實世界對應速查：**

| VFR 概念 | 現實對應 |
|---|---|
| Buffer Pool | pgBouncer 連線池、thread pool |
| refcount 歸零釋放 | C++ `shared_ptr` |
| DROP_OLDEST | Kafka earliest offset reset |
| BLOCK_PRODUCER | TCP 滑動視窗 flow control |
| SKIP_SELF | RxJava `onBackpressureDrop()` |
| Registry | Consul / etcd 服務發現 |
| eventfd + epoll | Node.js Event Loop |
| EFD_SEMAPHORE | Redis `LPUSH` / `BLPOP` |
| SCM_RIGHTS | Android Binder `FileDescriptor` |
| pidfd Watchdog | Kubernetes liveness probe |
| 環形緩衝器 | 行車記錄器、飛機黑盒子 |
| 狀態機 | TCP 狀態機、Redux |

---

*學習記錄：2026-05-06*
