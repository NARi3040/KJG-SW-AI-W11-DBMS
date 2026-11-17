/*
 * cli_faults.c - test-only fault shim for main.c regression tests
 *
 * Includes main.c with fread/fclose/signal renamed, then provides small
 * wrappers. This file is never linked into the production CLI.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SIGPIPE
#define SIGPIPE 13
#endif

#if defined(_WIN32)
typedef __p_sig_fn_t TestSignalHandler;
#else
typedef void (*TestSignalHandler)(int);
#endif

static size_t test_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
static int test_fclose(FILE *stream);
static TestSignalHandler test_signal(int signum, TestSignalHandler handler);

#define fread test_fread
#define fclose test_fclose
#define signal test_signal

#include "../main.c"

#undef fread
#undef fclose
#undef signal

static void write_marker(const char *path, const char *text) {
    FILE *file;

    if (path == NULL) {
        return;
    }
    file = fopen(path, "wb");
    if (file != NULL) {
        (void)fputs(text, file);
        (void)fclose(file);
    }
}

static size_t test_fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)stream;
    return 0;
}

static int test_fclose(FILE *stream) {
    static int calls;
    char text[16];
    int ignored = fclose(stream);

    (void)ignored;
    calls++;
    (void)snprintf(text, sizeof text, "%d\n", calls);
    write_marker(getenv("CLI_FAULT_CLOSE_MARKER"), text);
    return EOF;
}

static TestSignalHandler test_signal(int signum, TestSignalHandler handler) {
    static int calls;
    char text[16];

    (void)signum;
    (void)handler;
    calls++;
    (void)snprintf(text, sizeof text, "%d\n", calls);
    write_marker(getenv("CLI_FAULT_SIGNAL_MARKER"), text);
    if (getenv("CLI_FAULT_SIGNAL_STDERR") != NULL) {
        (void)fprintf(stderr, "signal\n");
    }
    return getenv("CLI_FAULT_SIGNAL_FAIL") != NULL ? SIG_ERR : SIG_IGN;
}
