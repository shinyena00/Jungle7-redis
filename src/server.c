#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "resp.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 6380
#define LISTEN_BACKLOG 128
#define EVENT_COUNT 64
#define READ_BUFFER_SIZE 4096
#define STORE_BUCKET_COUNT 8

typedef struct {
    char * data;
    size_t length;
    size_t capacity;
} buffer_t;

typedef struct {
    int descriptor;
    buffer_t input;
    buffer_t output;
    size_t sent_length;
    bool close_after_reply;
} client_t;

static int find_port(void);
static int open_listener(int port);
static int run_event_loop(int listener, store_t * store);
static void dispatch_events(int listener, int poller,
                            const struct epoll_event * events, int count,
                            store_t * store);
static void accept_client(int listener, int poller);
static void destroy_client(client_t * client);
static void handle_client_event(int poller, struct epoll_event event,
                                store_t * store);
static void write_client(int poller, client_t * client, store_t * store);
static bool is_retryable_io_error(void);
static void process_request(int poller, client_t * client, store_t * store);
static void update_client_interest(int poller, client_t * client,
                                   uint32_t events);
static buffer_t * copy_reply(buffer_t * output, const char * message);
static buffer_t * reserve_buffer(buffer_t * buffer, size_t length);
static int64_t read_now_ms(void);
static buffer_t * encode_reply(buffer_t * output, command_reply_t reply);
static buffer_t * encode_frame(buffer_t * output, const char * prefix,
                               command_reply_t reply);
static void read_client(int poller, client_t * client, store_t * store);

/* ── 공개 함수 ── */

int main (void) {
    int port = find_port();
    if (port < 0) {
        fputs("MINI_REDIS_PORT는 1~65535 사이여야 합니다.\n", stderr);
        return EXIT_FAILURE;
    }

    int listener = open_listener(port);
    if (listener < 0) {
        perror("listen");
        return EXIT_FAILURE;
    }

    store_t * store = store_new(STORE_BUCKET_COUNT);
    if (store == NULL) {
        close(listener);
        fputs("저장소를 만들 메모리가 부족합니다.\n", stderr);
        return EXIT_FAILURE;
    }

    int result = run_event_loop(listener, store);
    store_free(store);
    close(listener);
    return result;
}

static int find_port (void) {
    const char * setting = getenv("MINI_REDIS_PORT");
    if (setting == NULL) {
        return DEFAULT_PORT;
    }

    char * end;
    long port = strtol(setting, &end, 10);
    if (end == setting || *end != '\0' || port < 1 || port > UINT16_MAX) {
        return -1;
    }
    return (int)port;
}

static int open_listener (int port) {
    int descriptor = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                            0);
    if (descriptor < 0) {
        return -1;
    }

    int enabled = 1;
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof(enabled)) < 0 ||
        bind(descriptor, (const struct sockaddr *)&address,
             sizeof(address)) < 0 ||
        listen(descriptor, LISTEN_BACKLOG) < 0) {
        int error = errno;
        close(descriptor);
        errno = error;
        return -1;
    }
    return descriptor;
}

static int run_event_loop (int listener, store_t * store) {
    int poller = epoll_create1(EPOLL_CLOEXEC);
    if (poller < 0) {
        perror("epoll_create1");
        return EXIT_FAILURE;
    }

    struct epoll_event interest = {.events = EPOLLIN, .data.ptr = NULL};
    if (epoll_ctl(poller, EPOLL_CTL_ADD, listener, &interest) < 0) {
        perror("epoll_ctl");
        close(poller);
        return EXIT_FAILURE;
    }

    struct epoll_event events[EVENT_COUNT];
    for (;;) {
        int count = epoll_wait(poller, events, EVENT_COUNT, -1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            perror("epoll_wait");
            break;
        }
        dispatch_events(listener, poller, events, count, store);
    }
    close(poller);
    return EXIT_FAILURE;
}

static void dispatch_events (int listener, int poller,
                             const struct epoll_event * events, int count,
                             store_t * store) {
    for (int i = 0; i < count; i++) {
        if (events[i].data.ptr == NULL) {
            accept_client(listener, poller);
        } else {
            handle_client_event(poller, events[i], store);
        }
    }
}

static void accept_client (int listener, int poller) {
    int descriptor = accept4(listener, NULL, NULL,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (descriptor < 0) {
        return;
    }

    client_t * client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(descriptor);
        return;
    }
    client->descriptor = descriptor;

    struct epoll_event interest = {.events = EPOLLIN, .data.ptr = client};
    if (epoll_ctl(poller, EPOLL_CTL_ADD, descriptor, &interest) < 0) {
        destroy_client(client);
    }
}

static void destroy_client (client_t * client) {
    close(client->descriptor);
    free(client->input.data);
    free(client->output.data);
    free(client);
}

static void handle_client_event (int poller, struct epoll_event event,
                                 store_t * store) {
    client_t * client = event.data.ptr;
    if (event.events & (EPOLLERR | EPOLLHUP)) {
        destroy_client(client);
        return;
    }
    if (event.events & EPOLLOUT) {
        write_client(poller, client, store);
        return;
    }
    if (event.events & EPOLLIN) {
        read_client(poller, client, store);
    }
}

static void write_client (int poller, client_t * client, store_t * store) {
    const char * data = client->output.data + client->sent_length;
    size_t remaining = client->output.length - client->sent_length;
    ssize_t count = send(client->descriptor, data, remaining, MSG_NOSIGNAL);
    if (count < 0 && is_retryable_io_error()) {
        return;
    }
    if (count <= 0) {
        destroy_client(client);
        return;
    }

    client->sent_length += (size_t)count;
    if (client->sent_length < client->output.length) {
        return;
    }
    if (client->close_after_reply) {
        destroy_client(client);
        return;
    }

    client->sent_length = 0;
    client->output.length = 0;
    process_request(poller, client, store);
}

static bool is_retryable_io_error (void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static void process_request (int poller, client_t * client, store_t * store) {
    buffer_t * input = &client->input;
    resp_request_t request;
    resp_status_t status = parse_resp_request(input->data, input->length,
                                              &request);
    if (status == RESP_INCOMPLETE) {
        update_client_interest(poller, client, EPOLLIN);
        return;
    }
    if (status != RESP_COMPLETE) {
        const char * message = find_resp_error_reply(status);
        if (copy_reply(&client->output, message) == NULL) {
            destroy_client(client);
            return;
        }
        client->close_after_reply = true;
        update_client_interest(poller, client, EPOLLOUT);
        return;
    }

    int64_t now_ms = read_now_ms();
    if (now_ms < 0) {
        destroy_client(client);
        return;
    }
    command_reply_t reply = execute_command(store, request.arguments,
                                            request.argument_count, now_ms);
    /* 조회 결과는 다음 저장소 명령 전에 송신 버퍼에 복사한다. */
    if (encode_reply(&client->output, reply) == NULL) {
        destroy_client(client);
        return;
    }

    input->length -= request.consumed_length;
    memmove(input->data, input->data + request.consumed_length, input->length);
    update_client_interest(poller, client, EPOLLOUT);
}

static void update_client_interest (int poller, client_t * client,
                                    uint32_t events) {
    struct epoll_event interest = {.events = events, .data.ptr = client};
    if (epoll_ctl(poller, EPOLL_CTL_MOD, client->descriptor, &interest) < 0) {
        destroy_client(client);
    }
}

static buffer_t * copy_reply (buffer_t * output, const char * message) {
    size_t length = strlen(message);
    if (reserve_buffer(output, length) == NULL) {
        return NULL;
    }
    memcpy(output->data, message, length);
    output->length = length;
    return output;
}

static buffer_t * reserve_buffer (buffer_t * buffer, size_t length) {
    if (length <= buffer->capacity) {
        return buffer;
    }

    size_t capacity = buffer->capacity;
    if (capacity < READ_BUFFER_SIZE) {
        capacity = READ_BUFFER_SIZE;
    }
    while (capacity < length && capacity <= SIZE_MAX / 2) {
        capacity *= 2;
    }
    if (capacity < length) {
        capacity = length;
    }

    char * data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return NULL;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return buffer;
}

static int64_t read_now_ms (void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static buffer_t * encode_reply (buffer_t * output, command_reply_t reply) {
    switch (reply.kind) {
    case COMMAND_REPLY_OK:
        return copy_reply(output, "+OK\r\n");
    case COMMAND_REPLY_NIL:
        return copy_reply(output, "$-1\r\n");
    case COMMAND_REPLY_BULK: {
        char prefix[32];
        snprintf(prefix, sizeof(prefix), "$%zu\r\n", reply.length);
        return encode_frame(output, prefix, reply);
    }
    case COMMAND_REPLY_ERROR:
        return encode_frame(output, "-", reply);
    case COMMAND_REPLY_OUT_OF_MEMORY:
        return copy_reply(output, "-ERR 메모리가 부족합니다\r\n");
    }
    return NULL;
}

static buffer_t * encode_frame (buffer_t * output, const char * prefix,
                                command_reply_t reply) {
    size_t prefix_length = strlen(prefix);
    size_t length = prefix_length + reply.length + 2;
    if (reserve_buffer(output, length) == NULL) {
        return NULL;
    }
    memcpy(output->data, prefix, prefix_length);
    memcpy(output->data + prefix_length, reply.data, reply.length);
    memcpy(output->data + length - 2, "\r\n", 2);
    output->length = length;
    return output;
}

static void read_client (int poller, client_t * client, store_t * store) {
    char bytes[READ_BUFFER_SIZE];
    ssize_t count = recv(client->descriptor, bytes, sizeof(bytes), 0);
    if (count < 0 && is_retryable_io_error()) {
        return;
    }
    if (count <= 0) {
        destroy_client(client);
        return;
    }

    buffer_t * input = &client->input;
    if ((size_t)count > SIZE_MAX - input->length ||
        reserve_buffer(input, input->length + (size_t)count) == NULL) {
        destroy_client(client);
        return;
    }
    memcpy(input->data + input->length, bytes, (size_t)count);
    input->length += (size_t)count;
    process_request(poller, client, store);
}
