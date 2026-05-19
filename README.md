# 多執行緒 TCP Echo Server

以 C 語言實作的高效能 TCP Server，結合 **epoll** I/O 多路復用、**執行緒池（Thread Pool）** 與**有界任務佇列**，作為系統程式設計的實作參考，涵蓋事件驅動 I/O、執行緒同步、生產者消費者等面試常見主題。

---

## 系統架構

```
                   ┌─────────────────────────────────┐
                   │           主執行緒                │
                   │                                 │
  clients ──────►  │  epoll_wait()                   │
                   │    │                            │
                   │    ├─ 新連線                     │
                   │    │     accept() + EPOLL_ADD   │
                   │    │     (EPOLLET | EPOLLONESHOT)│
                   │    │                             │
                   │    └─ 可讀事件                    │
                   │          enqueue(fd)            │
                   └──────────────┬──────────────────┘
                                  │ 有界環狀佇列
                                  │ (mutex + 2x condvar)
                   ┌──────────────▼──────────────────┐
                   │         執行緒池（x4）            │
                   │                                 │
                   │  dequeue() → recv() → send()    │
                   │           → epoll_rearm()       │
                   └─────────────────────────────────┘
```

**設計決策說明：**

- **epoll Edge-Triggered (ET)** — O(1) 事件通知，只在狀態改變時觸發，每次事件必須把 fd 完全 drain 乾淨。
- **EPOLLONESHOT** — fd 觸發一次事件後自動從 epoll 停用，Worker 處理完後呼叫 `epoll_rearm()` 重新啟用，避免兩個 Worker 同時 race 同一個 fd。
- **有界佇列 + 雙 condvar** — `queue_not_empty` 讓閒置 Worker 等待任務；`queue_not_full` 在佇列滿時對主執行緒施加 back-pressure。
- **Non-blocking fd** — 所有 client socket 設為 `O_NONBLOCK`，`recv()` 回傳 `EAGAIN` 代表已 drain 完畢，此時 re-arm epoll。

---

## 功能特色

- epoll ET + `EPOLLONESHOT`，消除 fd race condition
- 固定大小執行緒池（預設 4 個 Worker）
- 有界環狀任務佇列，佇列滿時 Producer 自動等待
- `SO_REUSEADDR`，Server 重啟不卡 port
- 忽略 `SIGPIPE`，斷線回傳 `EPIPE` 而非 crash
- 收到 `SIGINT` / `SIGTERM` 時優雅關閉（Graceful Shutdown）

---

## 編譯與執行

```bash
# 編譯
gcc -o tcp_server tcp_server.c -lpthread

# 執行（監聽 port 8080）
./tcp_server
```

用 `nc` 快速驗證：

```bash
echo "hello" | nc 127.0.0.1 8080
# → hello
```

---

## 測試

測試套件涵蓋功能正確性與並發壓力測試。

```bash
gcc -o tcp_server_test tcp_server_test.c -lpthread
./tcp_server_test
```

| 套件 | 測試項目 |
|------|---------|
| **功能測試** | 單次 echo、64 KB 資料完整性、同連線多次收送、Client 斷線後 Server 存活 |
| **壓力測試** | 50 個 concurrent clients x 100 則訊息，驗證零錯誤與 throughput |

預期輸出：

```
[Functional] Single echo
  [PASS] connect
  [PASS] send / recv
  [PASS] echo correctness

[Stress] 50 concurrent clients x 100 messages each
  Total messages : 5000 / 5000
  Errors         : 0
  Wall time      : 0.31 s
  Throughput     : ~16000 msg/s

Results: 14 passed, 0 failed
```

---

## 檔案結構

```
.
├── tcp_server.c       # Server 實作
├── tcp_server_test.c  # 功能測試 + 壓力測試
└── README.md
```

---

## 涵蓋概念

| 概念 | 對應位置 |
|------|---------|
| epoll ET vs LT | `main()` 中的 `EPOLLET` flag + Worker 的 drain 迴圈 |
| EPOLLONESHOT | `epoll_ctl ADD/MOD` + `epoll_rearm()` |
| 生產者消費者模型 | `enqueue()` / `dequeue()` + mutex + condvar |
| 有界佇列 back-pressure | `enqueue()` 中的 `queue_not_full` condvar |
| Non-blocking I/O | `set_nonblocking()` + `EAGAIN` 處理 |
| Graceful Shutdown | `SIGINT` handler + `pthread_join` |
