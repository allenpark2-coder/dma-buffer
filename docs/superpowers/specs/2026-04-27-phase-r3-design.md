# Phase R3：異步寫入 + 分段 — 設計規格

*日期：2026-04-27*
*基於 plan-recorder-v1.4.1.md §九、§十一 Phase R3*

---

## 一、目標

實作可產生可播放 `.ts` 檔案的異步寫入引擎，不依賴任何外部 library（MPEG-TS 純 C 自行實作）。

---

## 二、模組結構

| 檔案 | 責任 | 依賴 |
|------|------|------|
| `rec/rec_ts_mux.c/.h` | PAT/PMT/PES/PCR 封裝，純函數，無 thread、無 I/O | 無外部依賴 |
| `rec/rec_writer.c/.h` | Writer thread、Write Queue 消費、pre_queue 消費、呼叫 mux、呼叫 segment | rec_ts_mux, rec_segment, rec_buf |
| `rec/rec_segment.c/.h` | 切檔判斷（時間/大小）、檔案命名、open/close、fdatasync | 無 |
| `test/test_rec_writer.c` | C headless 測試 | rec_writer, rec_ts_mux, rec_segment |

**Makefile 新增 targets：**
```
check_r3              # 純 C headless 測試（CI 必跑）
check_r3_ffprobe      # 寫 .ts + 呼叫 ffprobe（開發驗收）
test_rec_writer_asan  # ASan 版本
```

---

## 三、`rec_ts_mux` API

TS muxer 為純函數層，不持有狀態，continuity counter 由 caller 管理。

```c
/* rec_ts_mux.h */

#define REC_TS_PACKET_SIZE   188
#define REC_TS_PID_PAT       0x0000
#define REC_TS_PID_PMT       0x0100
#define REC_TS_PID_VIDEO     0x0101

/* 計算 rec_ts_write_pes 所需的輸出 buffer 大小 */
#define REC_TS_PES_BUF_SIZE(payload_size)  (((payload_size) / 184 + 2) * 188)

/* 回傳寫入的 byte 數；buf 由 caller 提供 */
int rec_ts_write_pat(uint8_t *buf, size_t buf_size, uint8_t *cc_pat);

int rec_ts_write_pmt(uint8_t *buf, size_t buf_size,
                     uint32_t format,   /* VFR_FMT_H264 or VFR_FMT_H265 */
                     uint8_t *cc_pmt);

/* 將一個 encoded frame 封裝成一個或多個 188-byte TS packets
 * pcr_ns : 目前的 PCR 時間戳（ns），插入首個 packet 的 adaptation field
 * pts_ns : frame PTS（ns），插入 PES header
 * is_keyframe : true → random_access_indicator 設 1
 */
int rec_ts_write_pes(uint8_t *buf, size_t buf_size,
                     const uint8_t *payload, uint32_t payload_size,
                     uint64_t pts_ns, uint64_t pcr_ns,
                     bool is_keyframe,
                     uint8_t *cc_video);
```

---

## 四、`rec_writer` 公開 API

```c
/* rec_writer.h */

typedef struct rec_writer rec_writer_t;

rec_writer_t *rec_writer_create(rec_buf_t *ring,
                                rec_codec_config_t *codec_cfg,
                                const char *output_dir,
                                const char *stream_name,
                                const rec_config_t *cfg);

/* event loop 持有此 fd，用於 EPOLL_CTL_ADD（EPOLLIN） */
int  rec_writer_get_eventfd(const rec_writer_t *w);

/* event loop 呼叫（live frame path）
 * item.data 所有權轉移給 writer；Writer 負責 free */
int  rec_writer_enqueue(rec_writer_t *w, rec_write_item_t item);

/* 送 shutdown sentinel → pthread_join → 釋放資源 → *w = NULL */
void rec_writer_destroy(rec_writer_t **w);
```

---

## 五、`rec_writer` 內部 Thread Loop

```
wait(writer_eventfd)

loop:
  [A] drain pre_extract_queue（優先）
      - batch 開始前 snapshot: abort_snapshot = atomic_load(aborted_pre_gen)
      - 每個 entry:
          - entry.batch_gen == abort_snapshot → free, skip, 繼續丟棄同 batch 殘留
          - 否則:
              tmp_buf = malloc(entry.size)
              memcpy(tmp_buf, ring + entry.offset, entry.size)
              if entry.batch_gen == abort_snapshot → free, skip
              else rec_ts_write_pes() → rec_segment_write()
              free(tmp_buf)
      - batch 全部 drain 完畢：
          if atomic_load(protected_gen) == pre_gen:
              清除 protect window（protected_gen → REC_PRE_GEN_NONE）

  [B] drain rec_write_queue
      - is_shutdown_sentinel → fdatasync + close + break
      - is_segment_boundary → 開新 segment（PAT + PMT + codec config + IDR）
      - 一般 frame → rec_ts_write_pes() → rec_segment_write()
      - free(item->data)
```

---

## 六、Codec Config 雙緩衝快照

- **event loop（寫）**：收到 config frame → 寫入非 active slot → release store 切 active_slot → version++
- **Writer（讀）**：每個 segment 起點 → `acquire load(active_slot)` → 複製到 thread-local 暫存 → 輸出 SPS/PPS（H.264）或 VPS/SPS/PPS（H.265）
- 若 cache 為空（`size == 0`）：不開新 segment，等到第一份 snapshot 到來

---

## 七、Segment 起點輸出順序

```
1. PAT
2. PMT
3. Codec Config NALUs（來自 codec config 快照）
4. IDR slice
```

---

## 八、`rec_segment` API

```c
typedef struct rec_segment rec_segment_t;

rec_segment_t *rec_segment_open(const char *output_dir,
                                const char *stream_name,
                                rec_mode_t mode);

/* 寫入已封裝好的 TS bytes
 * 回傳 0 = 繼續；1 = 時間或大小已達上限（caller 應在下一個 IDR 切段）*/
int  rec_segment_write(rec_segment_t *seg,
                       const uint8_t *ts_buf, size_t size,
                       uint64_t duration_ns);

/* fdatasync + close */
void rec_segment_close(rec_segment_t *seg);
```

**`rec_segment_write` 回傳 1 的觸發條件（任一成立）：**
- 累積錄影時間 ≥ `segment_duration_sec`（預設 600s）
- 累積檔案大小 ≥ `segment_size_max`（預設 2 GB）

**切段必須對齊 IDR（`pending_cut` 機制，由 `rec_writer.c` 管理）：**
```
若 rec_segment_write() 回傳 1：
  pending_cut = true
  繼續寫入當前 segment

下一個 frame 處理時：
  若 pending_cut && item.is_keyframe：
    rec_segment_close(seg)
    seg = rec_segment_open(...)
    寫入 PAT + PMT + codec config + IDR
    pending_cut = false
  若 pending_cut && !item.is_keyframe：
    繼續寫入舊 segment，等下一個 IDR
```

`is_segment_boundary == true` 的 item（overflow recovery 或 event clip 起點）由 `rec_writer.c` 直接觸發切段，不經過 `pending_cut` 路徑（item 本身已是 IDR）。

**檔案命名：**
```
<output_dir>/<stream_name>_<YYYYMMDD>_<HHMMSS>_<cont|event>.ts
```

**`fdatasync` 時機：**
1. segment 關閉前
2. 週期性 flush（`flush_interval_sec`，預設 60s，0 = 停用；由 rec_segment 內部追蹤）

---

## 九、Write Queue 溢出：drop-until-IDR

```
enqueue 失敗：
  overflow_drop = true
  drop_count++
  free(item.data)

後續每幀（overflow_drop == true）：
  if !is_keyframe → drop（free + drop_count++）
  if is_keyframe  → overflow_drop = false
                    is_segment_boundary = true
                    正常 enqueue
```

---

## 十、測試策略

### `test_rec_writer.c`（C headless，CI 必跑）

| Case | 驗證點 |
|------|--------|
| Live frame path | queue enqueue/drain，TS packet sync byte(0x47) + PES header 正確 |
| Segment boundary | PAT+PMT+codec config 在切段起點出現 |
| Codec config snapshot | Writer 讀到一致快照，不讀半包 |
| pre_extract_queue | Writer 從 ring 正確 memcpy，順序早於 live frame |
| Aborted batch | Writer 丟棄殘留 pre entry，不寫入 |
| Write Queue 溢出 | drop_count 遞增，不 crash，IDR 後恢復正常且 is_segment_boundary = true |
| Shutdown sentinel | Writer 正常 drain 並結束 thread |
| ASan | 無記憶體錯誤 |

### `check_r3_ffprobe`（Makefile 獨立 target）

合成假 SPS/PPS + IDR NAL，產生 `test_out.ts`，呼叫：
```bash
ffprobe -v error -show_entries format=duration -of csv=p=0 test_out.ts
```
驗 duration > 0 且 exit code 為 0。

---

## 十一、嚴格禁止事項（Phase R3 相關）

| 禁止行為 | 原因 |
|----------|------|
| rec_ts_mux 持有任何 static 狀態 | 破壞純函數性，難以測試 |
| Writer 把 pre_queue 資料 re-enqueue 至 Write Queue | 打破單一 producer，破壞時序 |
| Writer 在 dequeue 後立即清除 protect window | memcpy 未完成，ring 可能被覆蓋 |
| segment 起點不輸出 codec config NALUs | 播放器無法獨立解碼 |
| codec config 用 consume-once 布林 | 後續新 segment 缺 config |
| Write Queue 溢出時在非 IDR 邊界恢復寫入 | 解碼器花屏 |
