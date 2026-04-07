#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/epoll.h>

#define PORT         8080
#define MAX_EVENTS   64
#define BUFFER_SIZE  1024
#define THREAD_COUNT 4
#define QUEUE_SIZE   128

/* ------------------------------------------------------------------ */
/*  Task queue (circular buffer, bounded)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int client_fd;
    int epoll_fd;   /* worker needs this to re-arm EPOLLONESHOT */
} task_t;

static task_t           task_queue[QUEUE_SIZE];
static int              queue_front = 0, queue_rear = 0;
static pthread_mutex_t  queue_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   queue_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t   queue_not_full  = PTHREAD_COND_INITIALIZER;

/* Global flag for graceful shutdown */
static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------ */
/*  Queue helpers                                                       */
/* ------------------------------------------------------------------ */

static int queue_is_full(void) {
    return ((queue_rear + 1) % QUEUE_SIZE) == queue_front;
}

static int queue_is_empty(void) {
    return queue_front == queue_rear;
}

/*
 * Returns 0 on success, -1 if queue is full.
 * Caller should decide what to do (drop connection, back-pressure, etc.)
 */
static int enqueue(int client_fd, int epoll_fd) {
    pthread_mutex_lock(&queue_mutex);

    /* Wait until there is space (bounded blocking producer) */
    while (queue_is_full()) {
        pthread_cond_wait(&queue_not_full, &queue_mutex);
    }

    task_queue[queue_rear].client_fd = client_fd;
    task_queue[queue_rear].epoll_fd  = epoll_fd;
    queue_rear = (queue_rear + 1) % QUEUE_SIZE;

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

static task_t dequeue(void) {
    pthread_mutex_lock(&queue_mutex);

    while (queue_is_empty()) {
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }

    task_t task = task_queue[queue_front];
    queue_front = (queue_front + 1) % QUEUE_SIZE;

    pthread_cond_signal(&queue_not_full);
    pthread_mutex_unlock(&queue_mutex);
    return task;
}

/* ------------------------------------------------------------------ */
/*  Utility                                                             */
/* ------------------------------------------------------------------ */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Re-arm a fd in epoll with EPOLLONESHOT so the next event fires once.
 * Must be called after each worker finishes processing a fd.
 */
static int epoll_rearm(int epoll_fd, int client_fd) {
    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd  = client_fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &ev);
}

/* ------------------------------------------------------------------ */
/*  Worker thread                                                       */
/* ------------------------------------------------------------------ */

static void *worker(void *arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        task_t task = dequeue();
        int client_fd = task.client_fd;
        int epoll_fd  = task.epoll_fd;

        /*
         * ET mode: drain the fd completely until EAGAIN.
         * If we stop early, epoll won't fire again (ONESHOT + ET).
         */
        while (1) {
            ssize_t bytes = recv(client_fd, buffer, sizeof(buffer), 0);

            if (bytes > 0) {
                /* Echo back — replace with real protocol logic */
                ssize_t sent = 0;
                while (sent < bytes) {
                    ssize_t n = send(client_fd, buffer + sent, bytes - sent, 0);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                        perror("send");
                        break;
                    }
                    sent += n;
                }
            } else if (bytes == 0) {
                /* Client closed the connection */
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* No more data right now — re-arm and wait for next event */
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

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Graceful shutdown on SIGINT / SIGTERM */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    /* Ignore SIGPIPE so send() returns EPIPE instead of killing the process */
    signal(SIGPIPE, SIG_IGN);

    /* ---- Create server socket ---- */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR"); exit(EXIT_FAILURE);
    }

    if (set_nonblocking(server_fd) < 0) {
        perror("set_nonblocking server_fd"); exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen"); exit(EXIT_FAILURE);
    }

    /* ---- Create epoll instance ---- */
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); exit(EXIT_FAILURE); }

    struct epoll_event event;
    event.events  = EPOLLIN;
    event.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll_ctl ADD server_fd"); exit(EXIT_FAILURE);
    }

    /* ---- Spawn worker thread pool ---- */
    pthread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
            perror("pthread_create"); exit(EXIT_FAILURE);
        }
    }

    printf("Server running on port %d (workers: %d)\n", PORT, THREAD_COUNT);

    struct epoll_event events[MAX_EVENTS];

    while (running) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 500 /* ms timeout for shutdown check */);

        if (nfds < 0) {
            if (errno == EINTR) continue;  /* interrupted by signal, re-check running */
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == server_fd) {
                /* Accept all pending connections */
                while (1) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }

                    if (set_nonblocking(client_fd) < 0) {
                        perror("set_nonblocking client_fd");
                        close(client_fd);
                        continue;
                    }

                    struct epoll_event ev;
                    /*
                     * EPOLLET  — Edge-triggered: only notify on state change.
                     * EPOLLONESHOT — Disable fd after one event; worker re-arms it.
                     *   Together they prevent two workers from racing on the same fd.
                     */
                    ev.events  = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    ev.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        perror("epoll_ctl ADD client_fd");
                        close(client_fd);
                    }
                }
            } else {
                /* Readable client fd — hand off to thread pool */
                enqueue(fd, epoll_fd);
            }
        }
    }

    /* ---- Graceful shutdown ---- */
    printf("\nShutting down...\n");
    close(server_fd);
    close(epoll_fd);

    /*
     * Wake all workers so they can exit.
     * A production server would send a sentinel task or use pthread_cancel.
     * Here we broadcast and let them loop once more before process exits.
     */
    pthread_cond_broadcast(&queue_not_empty);

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    return 0;
}
