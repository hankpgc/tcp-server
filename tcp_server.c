#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define PORT 8080
#define MAX_EVENTS 64
#define BUFFER_SIZE 1024
#define THREAD_COUNT 4
#define QUEUE_SIZE 128

// ### Task queue (circular buffer, bounded) ###

typedef struct {
    int client_fd; // 這個任務要處理哪條連線
    int epoll_fd;  // worker 處理完要 re-arm，所以也要知道 epoll_fd
} task_t;

static task_t task_queue[QUEUE_SIZE]; // 環狀佇列本體，固定大小陣列
static int queue_front = 0,
           queue_rear = 0; // 下一個要取出的位置（Consumer 用）|| 下一個要放入的位置（Producer 用）
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
// 保護 queue，同一時間只有一個 thread 能進來操作

static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
// Worker 在這等，有任務時 Producer 叫醒

static pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;
// Producer 在這等，有空位時 Worker 叫醒

// Global flag for graceful shutdown
static volatile sig_atomic_t running =
    1; // sig_atomic_t：保證 signal handler 寫入這個值是原子操作，不會被打斷

static void handle_signal(int sig) {
    (void)sig;   // 避免「unused parameter」編譯警告
    running = 0; // 收到 SIGINT/SIGTERM 時把旗標設為 0，讓 main loop 結束
}

// ### Queue helpers ###

static int queue_is_full(void) { return ((queue_rear + 1) % QUEUE_SIZE) == queue_front; }

static int queue_is_empty(void) { return queue_front == queue_rear; }

static int enqueue(int client_fd, int epoll_fd) {
    pthread_mutex_lock(&queue_mutex);

    // 佇列滿了：釋放 mutex + 睡眠，等 Worker 消費後叫醒我
    // 用 while 不用 if：防止 spurious wakeup
    while (queue_is_full()) {
        pthread_cond_wait(&queue_not_full, &queue_mutex);
    }

    task_queue[queue_rear].client_fd = client_fd;
    task_queue[queue_rear].epoll_fd = epoll_fd;
    queue_rear = (queue_rear + 1) % QUEUE_SIZE;

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

static task_t dequeue(void) {
    pthread_mutex_lock(&queue_mutex);

    // 佇列空了：釋放 mutex + 睡眠，等 Producer 放入後叫醒我
    while (queue_is_empty()) {
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }

    task_t task = task_queue[queue_front];
    queue_front = (queue_front + 1) % QUEUE_SIZE;

    pthread_cond_signal(&queue_not_full);
    pthread_mutex_unlock(&queue_mutex);
    return task;
}

// 加上 O_NONBLOCK，之後 recv/send 不會 block，沒資料就回傳 EAGAIN
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 讓這個 fd 重新啟用，下次有資料才會再通知
static int epoll_rearm(int epoll_fd, int client_fd) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = client_fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
}

// ### Worker thread ###

static void *worker(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        task_t task = dequeue(); // 沒任務就睡眠在這裡
        int client_fd = task.client_fd;
        int epoll_fd = task.epoll_fd;

        while (1) { // ET 模式：drain 迴圈，把 fd 讀乾淨
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);

            if (bytes > 0) {
                // 讀到資料，echo 回去

                ssize_t sent = 0;
                while (sent < bytes) { // send 不保證一次送完，用迴圈確保全部送出
                    ssize_t n = send(client_fd, buffer + sent, bytes - sent, 0);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) // send buffer 暫時滿了，繼續重試
                            continue;
                        perror("send");
                        break;
                    }
                    sent += n;
                }
            } else if (bytes == 0) {
                // recv 回傳 0 = client 主動關閉連線
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 沒有更多資料了（drain 完畢）
                    // 重新啟用這個 fd，下次有資料再通知
                    if (epoll_rearm(epoll_fd, client_fd) < 0) {
                        perror("epoll_rearm");
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                        close(client_fd);
                    }
                    break;
                }
                perror("recv");
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
                break;
            }
        }
    }

    return NULL;
}

// ### Main ###

int main(void) {
    // Graceful shutdown on SIGINT / SIGTERM
    signal(SIGINT, handle_signal);  // Ctrl+C 時呼叫 handle_signal
    signal(SIGTERM, handle_signal); // kill 指令時呼叫 handle_signal

    // Ignore SIGPIPE so send() returns EPIPE instead of killing the process
    signal(SIGPIPE, SIG_IGN);

    // Create server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); // AF_INET = IPv4，SOCK_STREAM = TCP
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd) < 0) {
        perror("set_nonblocking server_fd");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),       // htons：host byte order → network byte order
        .sin_addr.s_addr = INADDR_ANY, // 監聽所有網路介面
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) { // 開始監聽，backlog=128 代表等待佇列最多容納 128 個未處理的連線
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // 建立 epoll 實例
    int epoll_fd = epoll_create1(0); // 建立 epoll 實例，kernel 內部建一張事件表
    if (epoll_fd < 0) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    struct epoll_event event;
    event.events = EPOLLIN; // 監控「可讀」事件（有新連線進來）
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl ADD server_fd");
        exit(EXIT_FAILURE);
    }

    // 建立 thread pool
    // 建立 4 個 worker thread，全部跑 worker()，全部卡在 dequeue() 睡眠等任務
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }

    printf("Server running on port %d (workers: %d)\n", PORT, THREAD_COUNT);

    struct epoll_event events[MAX_EVENTS];

    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 500); // ms timeout for shutdown check
        // 等待事件，500ms timeout：讓迴圈每 0.5 秒檢查一次 running
        // nfds = 有幾個 fd 有事件
        // 回傳的事件放在 events[] 陣列裡

        if (nfds < 0) {
            if (errno == EINTR)
                continue; // 被 signal 打斷，不是真的錯誤，繼續迴圈重新檢查 running
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                // 有新連線進來
                while (1) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("accept");
                        break;
                    }

                    if (set_nonblocking(client_fd) < 0) {
                        perror("set_nonblocking client_fd");
                        close(client_fd);
                        continue;
                    }

                    struct epoll_event ev;

                    // 把新的 client_fd 加入 epoll 監控
                    // EPOLLET = ET 模式
                    // EPOLLONESHOT = 觸發一次後自動停用，worker 處理完再 re-arm
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    ev.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        perror("epoll_ctl ADD client_fd");
                        close(client_fd);
                    }
                }
            } else {
                // client fd 有資料可讀，丟給 thread pool
                enqueue(fd, epoll_fd);
            }
        }
    }

    // Graceful shutdown
    printf("\nShutting down...\n");
    close(server_fd);
    close(epoll_fd);

    // 叫醒所有在 dequeue() 睡眠的 worker
    // 讓它們有機會跑到下一輪迴圈（生產環境應搭配 shutdown 旗標讓它們真正結束）
    pthread_cond_broadcast(&queue_not_empty);

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    // 等所有 worker 結束後，main 才 return
    // 確保資源全部釋放乾淨
    return 0;
}
