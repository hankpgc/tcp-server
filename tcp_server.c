#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/epoll.h>

#define PORT 8080
#define MAX_EVENTS 64
#define BUFFER_SIZE 1024
#define THREAD_COUNT 4
#define QUEUE_SIZE 128

typedef struct {
    int client_fd;
} task_t;

task_t task_queue[QUEUE_SIZE];
int queue_front = 0, queue_rear = 0;

pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;

void enqueue(int client_fd) {
    pthread_mutex_lock(&queue_mutex);

    task_queue[queue_rear].client_fd = client_fd;
    queue_rear = (queue_rear + 1) % QUEUE_SIZE;

    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_mutex);
}

int dequeue() {
    pthread_mutex_lock(&queue_mutex);

    while (queue_front == queue_rear)
        pthread_cond_wait(&queue_cond, &queue_mutex);

    int client_fd = task_queue[queue_front].client_fd;
    queue_front = (queue_front + 1) % QUEUE_SIZE;

    pthread_mutex_unlock(&queue_mutex);
    return client_fd;
}

void *worker(void *arg) {
    char buffer[BUFFER_SIZE];

    while (1) {
        int client_fd = dequeue();

        int bytes = recv(client_fd, buffer, BUFFER_SIZE, 0);

        if (bytes <= 0) {
            close(client_fd);
            continue;
        }

        send(client_fd, buffer, bytes, 0);
    }
}

int main() {

    int server_fd;
    struct sockaddr_in server_addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    listen(server_fd, 128);

    int epoll_fd = epoll_create1(0);

    struct epoll_event event, events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = server_fd;

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    pthread_t threads[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++)
        pthread_create(&threads[i], NULL, worker, NULL);

    printf("Server running on port %d\n", PORT);

    while (1) {

        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {

            if (events[i].data.fd == server_fd) {

                int client_fd = accept(server_fd, NULL, NULL);

                struct epoll_event ev;
                ev.events = EPOLLIN;
                ev.data.fd = client_fd;

                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
            }
            else {

                int client_fd = events[i].data.fd;

                enqueue(client_fd);
            }
        }
    }

    close(server_fd);
}