/*
 * tcp_server_test.c
 *
 * Build:
 *   gcc -o tcp_server_test tcp_server_test.c -lpthread
 *
 * Usage (server must be running first):
 *   ./tcp_server_test
 *
 * Test suites:
 *   1. Functional  — single connection, echo correctness, large payload,
 *                    server-side close detection
 *   2. Stress      — N concurrent clients, each sends M messages,
 *                    measures throughput and checks for data corruption
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <pthread.h>

#define SERVER_IP      "127.0.0.1"
#define SERVER_PORT    8080
#define STRESS_CLIENTS 50
#define STRESS_MSGS    100
#define MSG_SIZE       256

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(SERVER_PORT),
    };
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* send all bytes, return 0 on success */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* recv exact bytes, return 0 on success */
static int recv_all(int fd, char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* Test result counters */
static int passed = 0;
static int failed = 0;

#define ASSERT(cond, name)                                      \
    do {                                                        \
        if (cond) {                                             \
            printf("  [PASS] %s\n", name);                     \
            passed++;                                           \
        } else {                                                \
            printf("  [FAIL] %s  (line %d)\n", name, __LINE__);\
            failed++;                                           \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Suite 1: Functional tests                                          */
/* ------------------------------------------------------------------ */

static void test_single_echo(void) {
    printf("\n[Functional] Single echo\n");

    int fd = connect_to_server();
    ASSERT(fd >= 0, "connect");
    if (fd < 0) return;

    const char *msg = "hello server";
    size_t len = strlen(msg);
    char buf[64] = {0};

    ASSERT(send_all(fd, msg, len) == 0, "send");
    ASSERT(recv_all(fd, buf, len) == 0, "recv");
    ASSERT(memcmp(msg, buf, len) == 0,  "echo correctness");

    close(fd);
}

static void test_large_payload(void) {
    printf("\n[Functional] Large payload (64 KB)\n");

    int fd = connect_to_server();
    ASSERT(fd >= 0, "connect");
    if (fd < 0) return;

    size_t len = 64 * 1024;
    char *send_buf = malloc(len);
    char *recv_buf = malloc(len);

    /* Fill with a recognizable pattern */
    for (size_t i = 0; i < len; i++)
        send_buf[i] = (char)(i & 0xFF);

    ASSERT(send_all(fd, send_buf, len) == 0, "send 64 KB");
    ASSERT(recv_all(fd, recv_buf, len) == 0, "recv 64 KB");
    ASSERT(memcmp(send_buf, recv_buf, len) == 0, "data integrity");

    free(send_buf);
    free(recv_buf);
    close(fd);
}

static void test_multiple_messages(void) {
    printf("\n[Functional] Multiple messages on same connection\n");

    int fd = connect_to_server();
    ASSERT(fd >= 0, "connect");
    if (fd < 0) return;

    int ok = 1;
    for (int i = 0; i < 10; i++) {
        char msg[32], buf[32];
        int len = snprintf(msg, sizeof(msg), "message_%02d", i);

        if (send_all(fd, msg, len) < 0 || recv_all(fd, buf, len) < 0) {
            ok = 0; break;
        }
        if (memcmp(msg, buf, len) != 0) { ok = 0; break; }
    }
    ASSERT(ok, "10 sequential echo rounds");

    close(fd);
}

static void test_server_detects_close(void) {
    printf("\n[Functional] Server handles client disconnect\n");

    int fd = connect_to_server();
    ASSERT(fd >= 0, "connect");
    if (fd < 0) return;

    /* Send something, then immediately close */
    send_all(fd, "bye", 3);
    close(fd);

    /* Open a new connection — server should still be alive */
    usleep(50 * 1000);  /* 50ms: give server time to process */
    int fd2 = connect_to_server();
    ASSERT(fd2 >= 0, "server still accepting after client disconnect");

    char buf[4];
    send_all(fd2, "ok?", 3);
    ASSERT(recv_all(fd2, buf, 3) == 0, "server still echoes after disconnect");
    ASSERT(memcmp(buf, "ok?", 3) == 0, "echo correct after reconnect");

    close(fd2);
}

/* ------------------------------------------------------------------ */
/*  Suite 2: Stress test                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int    client_id;
    int    errors;
    int    messages_ok;
    double duration_sec;
} stress_result_t;

static void *stress_worker(void *arg) {
    stress_result_t *res = (stress_result_t *)arg;
    res->errors = 0;
    res->messages_ok = 0;

    int fd = connect_to_server();
    if (fd < 0) { res->errors++; return NULL; }

    char send_buf[MSG_SIZE], recv_buf[MSG_SIZE];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < STRESS_MSGS; i++) {
        /* Build a unique message per client/message */
        int len = snprintf(send_buf, sizeof(send_buf),
                           "client_%03d_msg_%04d_padding_XXXXXXXXXXXXXXXX",
                           res->client_id, i);

        if (send_all(fd, send_buf, len) < 0) { res->errors++; break; }
        if (recv_all(fd, recv_buf, len) < 0) { res->errors++; break; }
        if (memcmp(send_buf, recv_buf, len) != 0) { res->errors++; }
        else res->messages_ok++;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    res->duration_sec = (t1.tv_sec - t0.tv_sec)
                      + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    close(fd);
    return NULL;
}

static void test_stress(void) {
    printf("\n[Stress] %d concurrent clients × %d messages each\n",
           STRESS_CLIENTS, STRESS_MSGS);

    pthread_t       threads[STRESS_CLIENTS];
    stress_result_t results[STRESS_CLIENTS];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < STRESS_CLIENTS; i++) {
        results[i].client_id = i;
        pthread_create(&threads[i], NULL, stress_worker, &results[i]);
    }

    for (int i = 0; i < STRESS_CLIENTS; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Aggregate */
    int    total_ok  = 0, total_err = 0;
    double total_sec = (t1.tv_sec - t0.tv_sec)
                     + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    for (int i = 0; i < STRESS_CLIENTS; i++) {
        total_ok  += results[i].messages_ok;
        total_err += results[i].errors;
    }

    int expected = STRESS_CLIENTS * STRESS_MSGS;
    double throughput = total_ok / total_sec;

    printf("  Total messages : %d / %d\n", total_ok, expected);
    printf("  Errors         : %d\n", total_err);
    printf("  Wall time      : %.3f s\n", total_sec);
    printf("  Throughput     : %.0f msg/s\n", throughput);

    ASSERT(total_err == 0,          "zero errors across all clients");
    ASSERT(total_ok  == expected,   "all messages echoed correctly");
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("========================================\n");
    printf("  TCP Server Test Suite\n");
    printf("  Target: %s:%d\n", SERVER_IP, SERVER_PORT);
    printf("========================================\n");

    /* Quick connectivity check */
    int fd = connect_to_server();
    if (fd < 0) {
        fprintf(stderr, "\nERROR: Cannot connect to server. "
                        "Is tcp_server running?\n");
        return 1;
    }
    close(fd);

    /* --- Functional --- */
    test_single_echo();
    test_large_payload();
    test_multiple_messages();
    test_server_detects_close();

    /* --- Stress --- */
    test_stress();

    /* --- Summary --- */
    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", passed, failed);
    printf("========================================\n");

    return (failed > 0) ? 1 : 0;
}
