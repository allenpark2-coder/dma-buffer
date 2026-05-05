# Lock-Free 程式設計概念

## Lock-Free 是什麼？

傳統 mutex 的問題：

```c
// mutex 版本
pthread_mutex_lock(&lock);
slot->state = FILLING;      // 臨界區
pthread_mutex_unlock(&lock);
```

問題在於：**一個 thread 持有 lock 時，其他 thread 只能等待**。
如果持有 lock 的 thread 被 OS 排程暫停，其他所有 thread 都卡住。

Lock-free 的核心概念：
> **不用 lock，改用 CPU 提供的原子指令，讓多個 thread 可以同時嘗試，只有一個會成功，其他人重試。**

---

## 最核心的工具：CAS（Compare-And-Swap）

```c
// 意思是：
// 「如果 *ptr 現在是 expected，就把它換成 desired，回傳 true」
// 「如果不是，就把 expected 更新為現在的值，回傳 false」
atomic_compare_exchange_strong(ptr, &expected, desired);
```

這個操作在 **CPU 層面是不可分割的**，不需要 lock。

---

## 對比：搶 slot 的寫法

### mutex 版本

```c
pthread_mutex_lock(&pool->lock);
for (int i = 0; i < pool->slot_count; i++) {
    if (pool->slots[i].state == FREE) {
        pool->slots[i].state = FILLING;  // 搶到了
        pthread_mutex_unlock(&pool->lock);
        return i;
    }
}
pthread_mutex_unlock(&pool->lock);
return -1;
```

問題：整個 for 迴圈都在 lock 裡面，同一時間只有一個 thread 能搶。

### lock-free 版本

```c
for (int i = 0; i < pool->slot_count; i++) {
    int expected = FREE;
    // 原子地：如果 state == FREE，就換成 FILLING
    if (atomic_compare_exchange_strong(&slot->state, &expected, FILLING)) {
        return i;  // 搶到了
    }
    // 搶輸了？expected 已被更新為現在的值，繼續找下一個
}
return -1;
```

多個 thread 同時跑這個迴圈：
- Thread A 和 Thread B 同時對 slot[0] 做 CAS
- CPU 保證只有一個會成功
- 另一個 CAS 失敗，繼續往 slot[1] 找
- **沒有任何人在等待，大家都在工作**

---

## Lock-Free 的三個常用原子操作

```c
// 1. CAS：條件式交換（搶 slot 用）
atomic_compare_exchange_strong(&state, &expected, new_val);

// 2. fetch_sub：原子遞減，回傳舊值（refcount 用）
uint32_t prev = atomic_fetch_sub(&refcount, 1);
if (prev == 1) {
    // 我是最後一個，負責清理
}

// 3. atomic_store / atomic_load：單純的原子讀寫
atomic_store(&state, SLOT_FREE);
int s = atomic_load(&state);
```

---

## Memory Order

### 問題根源

你可能以為程式是「你寫什麼順序，CPU 就跑什麼順序」，但實際上不是。
CPU 和編譯器會偷偷重排指令來最佳化：

```c
// 你寫的
a = 1;
b = 2;

// CPU 實際執行可能是
b = 2;
a = 1;
```

**單執行緒沒問題**，但**多核心就出事了**。

### 具體出錯情境

```c
// 全域變數
int data   = 0;
int ready  = 0;

// Thread A（生產者）
data  = 42;    // ① 寫資料
ready = 1;     // ② 設旗子

// Thread B（消費者）
while (ready == 0) {}  // 等旗子
print(data);           // ③ 期望印出 42
```

**但 CPU 可能這樣執行：**

```
Thread A 實際順序：  ready = 1  →  data = 42
                        ↑
                   CPU 覺得這樣比較快，就換了順序

Thread B 看到：      ready = 1（但 data 還是 0！）
```

印出 0，bug。

---

### Memory Order 等級

#### `memory_order_relaxed` — 完全不管順序

```c
// 只保證這個變數本身是原子的，不管跟其他變數的順序
atomic_store_explicit(&counter, 1, memory_order_relaxed);
```

適合：純計數器，不需要跟其他變數同步。

#### `memory_order_release` + `memory_order_acquire` — 配對使用

```c
// Thread A（生產者）
data = 42;
// release：「在我之前的所有寫入，都必須在這行之前完成」
atomic_store_explicit(&ready, 1, memory_order_release);

// Thread B（消費者）
// acquire：「在我之後的所有讀取，都必須在這行之後才做」
while (atomic_load_explicit(&ready, memory_order_acquire) == 0) {}
print(data);  // 保證看到 42
```

比喻：

```
release = 「我打烊了，所有東西都收好才關門」
acquire = 「我開門了，門開了之後才能看裡面的東西」

Thread A 關門（release）→ Thread B 開門（acquire）
B 開門後，保證看到 A 關門前放好的所有東西
```

#### `memory_order_seq_cst` — 最強，全域一致順序

```c
atomic_store_explicit(&x, 1, memory_order_seq_cst);
```

所有 thread 看到的操作順序完全一樣，最安全但最慢。
這是 `atomic_store()` 不加 `_explicit` 時的預設值。

---

### vfr_pool 的實際例子

```c
// vfr_pool_begin_dispatch()
// ★ 必須先 release refcount，再 release state
// 確保其他 CPU 看到 IN_FLIGHT 時，refcount 一定已經是正確的值

atomic_store_explicit(&slot->refcount, n_consumers, memory_order_release);  // ①
atomic_store_explicit(&slot->state, SLOT_IN_FLIGHT, memory_order_release);  // ②
```

```c
// 消費者讀 state
int s = atomic_load_explicit(&slot->state, memory_order_acquire);  // ③
// ③ acquire 配對 ② release
// 保證：看到 IN_FLIGHT 之後，①的 refcount 也一定可見
uint32_t rc = atomic_load_explicit(&slot->refcount, memory_order_relaxed);  // 安全
```

### 同步關係圖

```
Thread A                          Thread B

data = 42        ─┐
                  │  這些寫入         看到 ready=1
atomic_store      │  全部完成    →    之後
  (release)      ─┘  才關門          才開門
                                      │
                            atomic_load (acquire) ─┐
                                                   │  這些讀取
                                                   │  全部在此之後
                            print(data) ──────────┘  保證看到 42
```

---

## 記憶口訣

| Order | 記法 | 用途 |
|-------|------|------|
| `relaxed` | 各自為政 | 純計數，不需同步 |
| `release` | 關門（寫） | 寫完資料後，設旗子 |
| `acquire` | 開門（讀） | 讀旗子後，再讀資料 |
| `seq_cst` | 全體一致 | 不想思考時的保險選擇 |

> `release` 和 `acquire` 一定要**配對**才有意義。
> 單獨用 `release` 或單獨用 `acquire` 是沒有效果的。

---

## 什麼時候用 Mutex，什麼時候用 Lock-Free？

| 情境 | 建議 |
|------|------|
| 臨界區邏輯複雜（多個變數要一起改） | Mutex |
| 單一變數的 increment / decrement | Lock-free（atomic） |
| 搶資源（slot、token） | Lock-free（CAS） |
| 需要等待條件（如 queue 空了要等） | Mutex + condvar |
| 高競爭、低延遲（如 frame buffer） | Lock-free |

> Lock-free 不是萬能的，它的代價是**程式碼複雜度高**，memory order 很容易寫錯。
> 簡單場景用 mutex 反而更清楚、更安全。
