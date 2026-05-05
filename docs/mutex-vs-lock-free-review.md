# Mutex vs Lock-Free 改進評估

## 系統特性

- 嵌入式 ARM（Ambarella CV72）
- 影像 frame buffer，對延遲敏感
- 多個 consumer 搶 slot
- Recording engine 有獨立 writer thread
- IPC 跨 process 傳 frame

---

## 各模組建議

| 模組 | 現況 | 建議 | 原因 |
|------|------|------|------|
| `vfr_pool` slot 搶佔 | Lock-free（CAS） | **維持** | 競爭模式單純，避免 ISR 路徑被卡住 |
| `vfr_pool` refcount | Lock-free（fetch_sub） | **維持** | 多 consumer 並行釋放，mutex 反而複雜 |
| `vfr_metrics` 計數器 | Lock-free（relaxed） | **維持** | 純計數，沒有同步需求，換 mutex 無意義 |
| `rec_writer` write queue | Lock-free queue | **改成 mutex + condvar** | IO bound，mutex 無效能損失，debug 容易 |
| `rec_buf` circular buffer | Lock-free + generation counter | **改成 mutex** | 邏輯過度複雜，正確性難驗證 |

---

## 詳細說明

### `vfr_pool` — 維持 Lock-free

slot 搶佔的競爭模式天生適合 CAS：多個 thread 同時搶 FREE slot，只有一個會成功，其他繼續找下一個。

用 mutex 的問題：camera ISR 或 driver callback 把 frame 推進來時，如果 mutex 被別的 thread 持有，**整個 acquire 就卡住了**，frame 可能漏掉。

refcount 的 `fetch_sub` 同理：多個 consumer 並行釋放，atomic decrement 比 mutex 保護的 decrement 更自然。

---

### `vfr_metrics` — 維持 Lock-free（relaxed）

純計數器，`relaxed` atomic 幾乎沒有代價。計數器本來就允許少量誤差，換成 mutex 只會引入不必要的競爭點。

---

### `rec_writer` write queue — 改成 Mutex + Condvar

**現在的問題：**
Writer thread 本來就要等 queue 有資料，lock-free 版本需要自己處理 busy-wait 或 eventfd 通知，複雜度高，難 debug。

**改成 mutex + condvar：**

```c
// enqueue（producer 端）
pthread_mutex_lock(&q->lock);
q->entries[q->tail] = entry;
q->tail = next;
pthread_cond_signal(&q->cond);
pthread_mutex_unlock(&q->lock);

// dequeue（writer thread）
pthread_mutex_lock(&q->lock);
while (queue_empty(q))
    pthread_cond_wait(&q->cond, &q->lock);
entry = q->entries[q->head];
pthread_mutex_unlock(&q->lock);
```

**為什麼沒有效能損失：**
Writer thread 的瓶頸在寫檔（IO bound），不在 queue 競爭。mutex 的 overhead 對整體效能幾乎沒有影響。

---

### `rec_buf` circular buffer — 改成 Mutex

**現在的問題：**

目前用 generation counter（`protected_gen`、`aborted_pre_gen`）在 lock-free 條件下保護「正在被 writer 讀取的區域不被 producer 覆蓋」。多個 atomic 變數互相協調，邏輯複雜，正確性難驗證。

**改成 mutex 之後：**

一把 mutex 保護 read/write offset，「不覆蓋保護區」的邏輯變成臨界區內的簡單判斷：

```
┌─────────────────────────────────┐
│  一把 mutex 保護 read/write offset │
│  write 進來 → 確認不覆蓋保護區  │
│  reader 標記保護區 → unlock      │
└─────────────────────────────────┘
```

generation counter 的複雜性消失，程式碼可讀性和正確性都大幅提升。

**注意事項：**
mutex 的粒度要控制好，不要把 IO 操作包進臨界區，否則會讓 producer 等太久。

---

## 改動工程量評估

| 模組 | 改動量 | 風險 |
|------|--------|------|
| `vfr_pool` | 無需改動 | — |
| `vfr_metrics` | 無需改動 | — |
| `rec_writer` | 中（替換 queue 實作） | 低，邏輯直觀 |
| `rec_buf` | 大（重新設計保護邏輯） | 中，需完整測試 |

---

## 原則總結

> **Lock-free 用在「競爭短暫、邏輯單純」的地方；Mutex 用在「邏輯複雜、需要等待」的地方。**

- 搶資源（slot、token）→ Lock-free（CAS）
- 純計數 → Lock-free（relaxed atomic）
- 有等待語意的 queue → Mutex + condvar
- 複雜的多變數協調邏輯 → Mutex
