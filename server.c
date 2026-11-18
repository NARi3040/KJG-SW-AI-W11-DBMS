/*
 * server.c - HTTP API 서버: POST /query로 받은 SQL을 실행하고 JSON으로 돌려줘요
 *
 * 메인 스레드는 연결만 받아 고정 크기 대기열에 넣고, 작업 스레드가 요청 하나를
 * 끝까지 처리한 뒤 연결을 닫아요. 전역 뮤텍스 하나가 execute() 호출만 감싸고,
 * parse_next()와 응답 전송은 잠금 밖에서 해요(docs/phase2-3.md 4장).
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* sysconf, pthread under strict -std=c11 */
#endif

#include "dbms.h"

/* winsock2.h는 windows.h보다 먼저 와야 해서 pthread.h보다 앞에 둬요. */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET Socket;
#define SOCKET_NONE INVALID_SOCKET
#define SOCKET_SHUT_WRITE SD_SEND
#define socket_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int Socket;
#define SOCKET_NONE (-1)
#define SOCKET_SHUT_WRITE SHUT_WR
#define socket_close close
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_PATH "users.sans"
#define BODY_MAX ((size_t)1 << 20)    /* 1 MiB */
#define HEADER_MAX 8192               /* 요청 줄과 헤더를 합친 최대 바이트 */
#define DRAIN_MAX (BODY_MAX * 4)      /* 거절한 요청에서 읽고 버릴 최대 바이트 */
#define DRAIN_TIMEOUT_MS 1000         /* 버리며 읽기 전체에 쓸 최대 시간 */
#define REJECT_MAX 64                 /* 503을 보내려고 기다릴 수 있는 연결 수 */

#define NOMEM_BODY "{\"ok\":false,\"results\":[],\"error\":{\"category\":\"memory\"," \
                   "\"message\":\"out of memory\"}}"

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct {
    Socket *items;
    size_t capacity;
    size_t head;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t ready;
} Queue;

typedef struct {
    Buffer *out;
    size_t count;
} RowSink;

typedef enum {
    REQUEST_OK,
    REQUEST_BAD,
    REQUEST_TOO_LARGE,
    REQUEST_FORBIDDEN, /* 브라우저에서 온 요청(CSRF 방어) */
    REQUEST_NOMEM,
    REQUEST_GONE /* 응답할 수 없음: 연결이 끊기거나 읽기 실패 */
} RequestStatus;

static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
static Storage *server_storage;
static Queue queue;
static Queue reject_queue; /* 대기열이 가득 찼을 때 503을 보낼 연결 */
/* "127.0.0.1:<포트>", "localhost:<포트>". 포트 80이면 클라이언트가 포트를 빼므로 이름만도 받아요. */
static char allowed_hosts[4][32];
static size_t allowed_host_count;

/* ---------- CLI와 같은 오류 문구 (main.c에서 복사) ---------- */

static const char *status_category(DbStatus status) {
    switch (status) {
    case DB_OK: return "ok";
    case DB_END: return "end";
    case DB_ERR_SYNTAX: return "syntax";
    case DB_ERR_ARG: return "argument";
    case DB_ERR_SCHEMA: return "schema";
    case DB_ERR_TYPE: return "type";
    case DB_ERR_RANGE: return "range";
    case DB_ERR_NOMEM: return "memory";
    case DB_ERR_IO: return "io";
    case DB_ERR_CORRUPT: return "corrupt";
    case DB_ERR_OUTPUT: return "output";
    case DB_ERR_WRITE_UNCERTAIN: return "write-uncertain";
    }
    return "internal";
}

static const char *status_cause(DbStatus status) {
    switch (status) {
    case DB_ERR_SYNTAX: return "invalid SQL";
    case DB_ERR_ARG: return "unsupported command";
    case DB_ERR_SCHEMA: return "invalid table, column, or value count";
    case DB_ERR_TYPE: return "value type mismatch";
    case DB_ERR_RANGE: return "value out of range";
    case DB_ERR_NOMEM: return "out of memory";
    case DB_ERR_IO: return "storage I/O failure";
    case DB_ERR_CORRUPT: return "storage file is corrupt";
    case DB_ERR_OUTPUT: return "cannot write output";
    case DB_ERR_WRITE_UNCERTAIN: return "write result uncertain; not retried";
    default: return "unexpected status";
    }
}

/* 입력 잘못은 400, 서버·저장 쪽 문제는 500이에요(docs/phase2-3.md 4.3절). */
static int http_status_of(DbStatus status) {
    switch (status) {
    case DB_ERR_SYNTAX:
    case DB_ERR_ARG:
    case DB_ERR_SCHEMA:
    case DB_ERR_TYPE:
    case DB_ERR_RANGE:
        return 400;
    default:
        return 500;
    }
}

/* ---------- 응답 버퍼 ---------- */

/* 실패하면 0이고 버퍼는 그대로예요. 성공하면 data는 늘 '\0'으로 끝나요. */
static int buffer_append(Buffer *buffer, const char *text, size_t length) {
    size_t needed;

    if (length > SIZE_MAX - buffer->length - 1) {
        return 0;
    }
    needed = buffer->length + length + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity != 0 ? buffer->capacity : 256;
        char *grown;

        while (capacity < needed) {
            capacity = capacity > SIZE_MAX / 2 ? needed : capacity * 2;
        }
        grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            return 0;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (length != 0) {
        memcpy(buffer->data + buffer->length, text, length);
    }
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int buffer_text(Buffer *buffer, const char *text) {
    return buffer_append(buffer, text, strlen(text));
}

static int buffer_int(Buffer *buffer, long long value) {
    char digits[32];
    int length = snprintf(digits, sizeof digits, "%lld", value);

    return length > 0 && buffer_append(buffer, digits, (size_t)length);
}

/* 따옴표·역슬래시·제어 문자만 이스케이프해요. UTF-8 검사는 하지 않아요. */
static int buffer_json_string(Buffer *buffer, const char *text) {
    const char *run = text;
    const char *p;

    if (!buffer_text(buffer, "\"")) {
        return 0;
    }
    for (p = text; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        char escape[8];
        int length;

        if (c != '"' && c != '\\' && c >= 0x20) {
            continue;
        }
        length = c == '"' || c == '\\' ? snprintf(escape, sizeof escape, "\\%c", c)
                                       : snprintf(escape, sizeof escape, "\\u%04x", c);
        if (!buffer_append(buffer, run, (size_t)(p - run)) ||
            !buffer_append(buffer, escape, (size_t)length)) {
            return 0;
        }
        run = p + 1;
    }
    return buffer_append(buffer, run, (size_t)(p - run)) && buffer_text(buffer, "\"");
}

/* execute() 안에서 잠금을 잡은 채 불려요. 소켓에 쓰지 않고 버퍼에만 모아요. */
static DbStatus append_row(const Row *row, void *user_data) {
    RowSink *sink = user_data;
    Buffer *out = sink->out;

    if ((sink->count != 0 && !buffer_text(out, ",")) || !buffer_text(out, "{\"id\":") ||
        !buffer_int(out, row->id) || !buffer_text(out, ",\"name\":") ||
        !buffer_json_string(out, row->name) || !buffer_text(out, ",\"age\":") ||
        !buffer_int(out, row->age) || !buffer_text(out, "}")) {
        return DB_ERR_NOMEM;
    }
    sink->count++;
    return DB_OK;
}

/* ---------- SQL 실행 ---------- */

/*
 * 문장을 첫 오류까지 차례로 실행하고 응답 JSON을 out에 써요. HTTP 상태를
 * 돌려주고, 응답을 만들 메모리가 모자라면 0이에요(앞 문장은 이미 저장됐을 수 있음).
 */
static int run_sql(Storage *storage, const char *sql, size_t length, Buffer *out) {
    Buffer results = {NULL, 0, 0};
    size_t cursor = 0;
    size_t statement = 1;
    int http_status = 200;
    const char *category = NULL;
    const char *message = NULL;
    char error[PARSE_ERROR_SIZE];
    int ok = 1;

    for (;;) {
        size_t previous = cursor;
        size_t mark = results.length;
        Command command;
        ExecuteResult result;
        DbStatus status;

        error[0] = '\0';
        status = parse_next(sql, length, &cursor, &command, error);
        if (status == DB_END) {
            break;
        }
        if (status != DB_OK) {
            http_status = http_status_of(status);
            category = status_category(status);
            message = status == DB_ERR_SYNTAX && error[0] != '\0' ? error
                                                                  : status_cause(status);
            break;
        }
        if (cursor <= previous || cursor > length) {
            http_status = 500;
            category = "internal";
            message = "parser returned an invalid cursor";
            break;
        }

        if (mark != 0 && !buffer_text(&results, ",")) {
            ok = 0;
            break;
        }
        memset(&result, 0, sizeof result);
        if (command.type == CMD_SELECT) {
            RowSink sink;

            sink.out = &results;
            sink.count = 0;
            if (!buffer_text(&results, "{\"type\":\"select\",\"rows\":[")) {
                ok = 0;
                break;
            }
            pthread_mutex_lock(&db_lock);
            status = execute(storage, &command, append_row, &sink, &result);
            pthread_mutex_unlock(&db_lock);
            if (status == DB_OK && !buffer_text(&results, "]}")) {
                ok = 0;
                break;
            }
        } else {
            pthread_mutex_lock(&db_lock);
            status = execute(storage, &command, NULL, NULL, &result);
            pthread_mutex_unlock(&db_lock);
            if (status == DB_OK && (!buffer_text(&results, "{\"type\":\"insert\",\"id\":") ||
                                    !buffer_int(&results, result.insert.id) ||
                                    !buffer_text(&results, "}"))) {
                ok = 0;
                break;
            }
        }
        if (status != DB_OK) {
            /* 실패한 문장의 결과(SELECT가 모은 행 포함)는 버려요. */
            results.length = mark;
            if (results.data != NULL) {
                results.data[mark] = '\0';
            }
            http_status = http_status_of(status);
            category = status_category(status);
            message = status_cause(status);
            break;
        }
        statement++;
    }

    if (ok) {
        ok = buffer_text(out, category == NULL ? "{\"ok\":true,\"results\":["
                                               : "{\"ok\":false,\"results\":[") &&
             buffer_append(out, results.data, results.length) && buffer_text(out, "]");
    }
    if (ok && category != NULL) {
        ok = buffer_text(out, ",\"error\":{\"statement\":") &&
             buffer_int(out, (long long)statement) &&
             buffer_text(out, ",\"category\":") && buffer_json_string(out, category) &&
             buffer_text(out, ",\"message\":") && buffer_json_string(out, message) &&
             buffer_text(out, "}");
    }
    ok = ok && buffer_text(out, "}");
    free(results.data);
    return ok ? http_status : 0;
}

/* ---------- 소켓 입출력 ---------- */

static long socket_recv(Socket socket, char *data, size_t length) {
#ifdef _WIN32
    return recv(socket, data, (int)(length > INT_MAX ? INT_MAX : length), 0);
#else
    ssize_t received;

    do {
        received = recv(socket, data, length, 0);
    } while (received < 0 && errno == EINTR);
    return (long)received;
#endif
}

static int send_all(Socket socket, const char *data, size_t length) {
    while (length > 0) {
        size_t chunk = length > (size_t)INT_MAX ? (size_t)INT_MAX : length;
#ifdef _WIN32
        int sent = send(socket, data, (int)chunk, 0);
#else
        ssize_t sent = send(socket, data, chunk, 0);

        if (sent < 0 && errno == EINTR) {
            continue;
        }
#endif
        if (sent <= 0) {
            return 0;
        }
        data += sent;
        length -= (size_t)sent;
    }
    return 1;
}

static const char *reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static void send_response(Socket socket, int status, const char *body, size_t length) {
    char head[192];
    int head_length = snprintf(head, sizeof head,
                               "HTTP/1.1 %d %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: %" PRIu64 "\r\n"
                               "Connection: close\r\n\r\n",
                               status, reason_phrase(status), (uint64_t)length);

    if (head_length > 0 && (size_t)head_length < sizeof head &&
        send_all(socket, head, (size_t)head_length)) {
        send_all(socket, body, length);
    }
}

/* 문장을 실행하기 전에 거절할 때 보내요. message는 이스케이프가 필요 없는 상수예요. */
static void send_request_error(Socket socket, int status, const char *message) {
    char body[128];
    int length = snprintf(body, sizeof body,
                          "{\"ok\":false,\"results\":[],\"error\":{\"category\":\"request\","
                          "\"message\":\"%s\"}}",
                          message);

    if (length > 0 && (size_t)length < sizeof body) {
        send_response(socket, status, body, (size_t)length);
    }
}

/* 버리며 읽을 때 멈춘 클라이언트가 스레드를 오래 붙잡지 않게 해요. */
static void set_receive_timeout(Socket socket, int milliseconds) {
#ifdef _WIN32
    DWORD timeout = (DWORD)milliseconds;
#else
    struct timeval timeout;

    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
#endif
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
}

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/* 읽지 않은 본문이 남은 채 닫으면 RST로 응답이 사라질 수 있어 조금 읽고 버려요.
 * 조금씩 보내는 클라이언트가 스레드를 붙잡지 않게 전체 시간도 제한해요(최대 약 2배). */
static void drain_and_close(Socket socket) {
    char scrap[4096];
    size_t drained = 0;
    long received;
    uint64_t deadline = monotonic_ms() + DRAIN_TIMEOUT_MS;

    shutdown(socket, SOCKET_SHUT_WRITE);
    set_receive_timeout(socket, DRAIN_TIMEOUT_MS);
    while (drained < DRAIN_MAX && monotonic_ms() < deadline &&
           (received = socket_recv(socket, scrap, sizeof scrap)) > 0) {
        drained += (size_t)received;
    }
    socket_close(socket);
}

/* ---------- HTTP 요청 읽기 ---------- */

/* text[0..length)가 소문자 lower와 대소문자 무시하고 같으면 1이에요. */
static int ascii_equals(const char *text, size_t length, const char *lower) {
    size_t i;

    if (strlen(lower) != length) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        char c = text[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != lower[i]) {
            return 0;
        }
    }
    return 1;
}

/* 앞뒤 공백·탭을 뺀 값을 [*start, *end)로 돌려줘요. */
static void trim_value(const char **start, const char **end) {
    while (*start < *end && (**start == ' ' || **start == '\t')) {
        (*start)++;
    }
    while (*end > *start && ((*end)[-1] == ' ' || (*end)[-1] == '\t')) {
        (*end)--;
    }
}

/* 10진수만 받아요. BODY_MAX를 넘으면 *out_too_large를 1로 해요. */
static int parse_content_length(const char *start, const char *end, size_t *out_length,
                                int *out_too_large) {
    size_t value = 0;

    trim_value(&start, &end);
    if (start == end) {
        return 0;
    }
    *out_too_large = 0;
    for (; start < end; start++) {
        if (*start < '0' || *start > '9') {
            return 0;
        }
        if (!*out_too_large) {
            value = value * 10 + (size_t)(*start - '0');
            if (value > BODY_MAX) {
                *out_too_large = 1;
            }
        }
    }
    *out_length = value;
    return 1;
}

static const char *find_header_end(const char *data, size_t length) {
    size_t i;

    for (i = 0; i + 4 <= length; i++) {
        if (memcmp(data + i, "\r\n\r\n", 4) == 0) {
            return data + i;
        }
    }
    return NULL;
}

/*
 * 요청 줄은 "POST /query HTTP/1.x"만 받아요. Content-Length 필수, chunked는
 * 거절해요. Expect: 100-continue면 본문을 받기 전에 100을 보내요(curl이 큰 본문에 씀).
 */
static RequestStatus read_request(Socket socket, char **out_body, size_t *out_length) {
    char header[HEADER_MAX];
    size_t received = 0;
    size_t searched;
    const char *header_end = NULL;
    const char *line;
    const char *line_end;
    size_t content_length = 0;
    int has_length = 0;
    int too_large = 0;
    int expect_continue = 0;
    int forbidden = 0;
    size_t leftover;
    char *body;

    while (header_end == NULL) {
        long n;

        if (received == sizeof header) {
            return REQUEST_BAD;
        }
        n = socket_recv(socket, header + received, sizeof header - received);
        if (n <= 0) {
            return REQUEST_GONE;
        }
        /* 앞에서 본 바이트는 다시 찾지 않아요. 경계에 걸친 3바이트만 다시 봐요. */
        searched = received > 3 ? received - 3 : 0;
        received += (size_t)n;
        header_end = find_header_end(header + searched, received - searched);
    }

    line = header;
    for (line_end = line; memcmp(line_end, "\r\n", 2) != 0; line_end++) {
    }
    if ((size_t)(line_end - line) != 20 || memcmp(line, "POST /query HTTP/1.", 19) != 0 ||
        (line[19] != '0' && line[19] != '1')) {
        return REQUEST_BAD;
    }

    while (line_end < header_end) {
        const char *colon;

        line = line_end + 2;
        for (line_end = line; memcmp(line_end, "\r\n", 2) != 0; line_end++) {
        }
        colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon == NULL) {
            return REQUEST_BAD;
        }
        if (ascii_equals(line, (size_t)(colon - line), "content-length")) {
            if (has_length ||
                !parse_content_length(colon + 1, line_end, &content_length, &too_large)) {
                return REQUEST_BAD;
            }
            has_length = 1;
        } else if (ascii_equals(line, (size_t)(colon - line), "transfer-encoding")) {
            return REQUEST_BAD;
        } else if (ascii_equals(line, (size_t)(colon - line), "expect")) {
            const char *value = colon + 1;
            const char *value_end = line_end;

            trim_value(&value, &value_end);
            expect_continue = ascii_equals(value, (size_t)(value_end - value), "100-continue");
        } else if (ascii_equals(line, (size_t)(colon - line), "origin")) {
            /* 브라우저는 POST에 Origin을 붙이고 curl·Python 클라이언트는 붙이지 않아요. */
            forbidden = 1;
        } else if (ascii_equals(line, (size_t)(colon - line), "host")) {
            /* DNS 리바인딩으로 들어온 요청은 Host가 다른 이름이에요. */
            const char *value = colon + 1;
            const char *value_end = line_end;
            size_t i;
            int allowed = 0;

            trim_value(&value, &value_end);
            for (i = 0; i < allowed_host_count; i++) {
                allowed |= ascii_equals(value, (size_t)(value_end - value), allowed_hosts[i]);
            }
            if (!allowed) {
                forbidden = 1;
            }
        }
    }
    if (forbidden) {
        return REQUEST_FORBIDDEN;
    }
    if (!has_length) {
        return REQUEST_BAD;
    }
    if (too_large) {
        return REQUEST_TOO_LARGE;
    }

    body = malloc(content_length + 1);
    if (body == NULL) {
        return REQUEST_NOMEM;
    }
    leftover = received - (size_t)(header_end + 4 - header);
    if (leftover > content_length) {
        leftover = content_length;
    }
    memcpy(body, header_end + 4, leftover);
    if (expect_continue && leftover < content_length &&
        !send_all(socket, "HTTP/1.1 100 Continue\r\n\r\n", 25)) {
        free(body);
        return REQUEST_GONE;
    }
    while (leftover < content_length) {
        long n = socket_recv(socket, body + leftover, content_length - leftover);

        if (n <= 0) {
            free(body);
            return REQUEST_GONE;
        }
        leftover += (size_t)n;
    }
    body[content_length] = '\0';
    *out_body = body;
    *out_length = content_length;
    return REQUEST_OK;
}

/* ---------- 작업 스레드와 대기열 ---------- */

static void handle_client(Socket socket) {
    char *body = NULL;
    size_t length = 0;
    Buffer out = {NULL, 0, 0};
    int status;

    switch (read_request(socket, &body, &length)) {
    case REQUEST_OK:
        status = run_sql(server_storage, body, length, &out);
        if (status == 0) {
            send_response(socket, 500, NOMEM_BODY, sizeof NOMEM_BODY - 1);
        } else {
            send_response(socket, status, out.data, out.length);
        }
        socket_close(socket);
        break;
    case REQUEST_BAD:
        send_request_error(socket, 400, "malformed request");
        drain_and_close(socket);
        break;
    case REQUEST_FORBIDDEN:
        send_request_error(socket, 403, "forbidden");
        drain_and_close(socket);
        break;
    case REQUEST_TOO_LARGE:
        send_request_error(socket, 413, "body too large");
        drain_and_close(socket);
        break;
    case REQUEST_NOMEM:
        send_response(socket, 500, NOMEM_BODY, sizeof NOMEM_BODY - 1);
        drain_and_close(socket);
        break;
    case REQUEST_GONE:
        socket_close(socket);
        break;
    }
    free(body);
    free(out.data);
}

/* 가득 차 있으면 0이에요. 기다리지 않아요. */
static int queue_push(Queue *q, Socket socket) {
    int pushed = 0;

    pthread_mutex_lock(&q->lock);
    if (q->count < q->capacity) {
        q->items[(q->head + q->count) % q->capacity] = socket;
        q->count++;
        pushed = 1;
        pthread_cond_signal(&q->ready);
    }
    pthread_mutex_unlock(&q->lock);
    return pushed;
}

static Socket queue_pop(Queue *q) {
    Socket socket;

    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        pthread_cond_wait(&q->ready, &q->lock);
    }
    socket = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return socket;
}

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        handle_client(queue_pop(&queue));
    }
    return NULL;
}

/* 요청을 읽지 않고 닫으면 클라이언트가 503 대신 RST를 볼 수 있어, 작업 스레드와
 * 메인 스레드를 막지 않는 별도 스레드가 503을 보내고 남은 요청을 읽고 버려요. */
static void *reject_main(void *arg) {
    (void)arg;
    for (;;) {
        Socket socket = queue_pop(&reject_queue);

        send_request_error(socket, 503, "server busy");
        drain_and_close(socket);
    }
    return NULL;
}

static int queue_init(Queue *q, size_t capacity) {
    q->items = malloc(capacity * sizeof *q->items);
    q->capacity = capacity;
    q->head = 0;
    q->count = 0;
    if (q->items == NULL) {
        return 0;
    }
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        return 0;
    }
    return pthread_cond_init(&q->ready, NULL) == 0;
}

/* accept가 EMFILE 같은 이유로 계속 실패할 때 CPU를 다 쓰지 않게 잠깐 쉬어요. */
static void pause_after_accept_error(void) {
#ifdef _WIN32
    Sleep(10);
#else
    struct timespec pause = {0, 10 * 1000 * 1000};

    nanosleep(&pause, NULL);
#endif
}

/* ---------- 실행 인자와 시작 ---------- */

/* 부호·공백 없는 10진수이고 [min, max] 안이면 1이에요. */
static int parse_count(const char *text, long min, long max, long *out_value) {
    char *end;
    long value;

    if (text == NULL || *text < '0' || *text > '9') {
        return 0;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max) {
        return 0;
    }
    *out_value = value;
    return 1;
}

static long default_threads(void) {
    long count;
#ifdef _WIN32
    SYSTEM_INFO info;

    GetSystemInfo(&info);
    count = (long)info.dwNumberOfProcessors;
#else
    count = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (count < 1) {
        return 4;
    }
    return count > 256 ? 256 : count;
}

static Socket open_listener(long port) {
    Socket listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;

    if (listener == SOCKET_NONE) {
        return SOCKET_NONE;
    }
#ifndef _WIN32
    {
        int on = 1;

        /* 다시 켤 때 TIME_WAIT 때문에 bind가 실패하지 않게 해요. */
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    }
#endif
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
        socket_close(listener);
        return SOCKET_NONE;
    }
    return listener;
}

static int usage(const char *program) {
    fprintf(stderr, "usage: %s [--port 1-65535] [--threads 1-256] [--queue 1-4096]\n",
            program);
    return 1;
}

int main(int argc, char *argv[]) {
    long port = 8080;
    long threads = default_threads();
    long capacity = 128;
    Socket listener;
    DbStatus status;
    long i;
    int argi;
#ifdef _WIN32
    WSADATA wsa;
#endif

    for (argi = 1; argi < argc; argi += 2) {
        const char *value = argi + 1 < argc ? argv[argi + 1] : NULL;
        int valid;

        if (strcmp(argv[argi], "--port") == 0) {
            valid = parse_count(value, 1, 65535, &port);
        } else if (strcmp(argv[argi], "--threads") == 0) {
            valid = parse_count(value, 1, 256, &threads);
        } else if (strcmp(argv[argi], "--queue") == 0) {
            valid = parse_count(value, 1, 4096, &capacity);
        } else {
            valid = 0;
        }
        if (!valid) {
            return usage(argv[0]);
        }
    }

#ifdef SIGPIPE
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "start: cannot ignore SIGPIPE\n");
        return 1;
    }
#endif
#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "start: cannot initialize Winsock\n");
        return 1;
    }
#endif

    /* 저장소는 작업 스레드를 만들기 전에 열어요(docs/phase2-3.md 4.5절). */
    status = storage_open(STORAGE_PATH, &server_storage);
    if (status != DB_OK) {
        fprintf(stderr, "open: %s\n", status_cause(status));
        return 1;
    }

    snprintf(allowed_hosts[0], sizeof allowed_hosts[0], "127.0.0.1:%ld", port);
    snprintf(allowed_hosts[1], sizeof allowed_hosts[1], "localhost:%ld", port);
    allowed_host_count = 2;
    if (port == 80) {
        strcpy(allowed_hosts[2], "127.0.0.1");
        strcpy(allowed_hosts[3], "localhost");
        allowed_host_count = 4;
    }
    listener = open_listener(port);
    if (listener == SOCKET_NONE) {
        fprintf(stderr, "start: cannot listen on 127.0.0.1:%ld\n", port);
        storage_close(server_storage);
        return 1;
    }

    if (!queue_init(&queue, (size_t)capacity) || !queue_init(&reject_queue, REJECT_MAX)) {
        fprintf(stderr, "start: cannot create request queue\n");
        socket_close(listener);
        storage_close(server_storage);
        return 1;
    }
    for (i = 0; i <= threads; i++) {
        pthread_t thread;

        /* 마지막 하나는 503 전송 스레드예요. */
        if (pthread_create(&thread, NULL, i < threads ? worker_main : reject_main, NULL) != 0) {
            /* 만든 스레드는 대기열에서 기다리기만 해서 저장소를 닫아도 안전해요. */
            fprintf(stderr, "start: cannot create worker thread\n");
            socket_close(listener);
            storage_close(server_storage);
            return 1;
        }
        pthread_detach(thread);
    }

    printf("listening on http://127.0.0.1:%ld (threads=%ld, queue=%ld)\n", port, threads,
           capacity);
    fflush(stdout);

    /* 종료 처리는 두지 않아요. Ctrl+C로 꺼도 쓰기마다 fflush하고 헤더를 INSERT마다
     * 갱신하므로 데이터와 다음 id가 남아요(docs/phase2-3.md 8장). */
    for (;;) {
        Socket client = accept(listener, NULL, NULL);

        if (client == SOCKET_NONE) {
            pause_after_accept_error();
            continue;
        }
        if (!queue_push(&queue, client) && !queue_push(&reject_queue, client)) {
            /* 503 대기열까지 차면 여기서 바로 보내요. 새 연결의 송신 버퍼는 비어 있어
             * 짧은 응답은 accept를 막지 않아요. 요청을 읽지 않고 닫으므로 클라이언트가
             * 이미 요청을 보냈다면 503 대신 연결 리셋을 볼 수 있어요. */
            send_request_error(client, 503, "server busy");
            shutdown(client, SOCKET_SHUT_WRITE);
            socket_close(client);
        }
    }
}
