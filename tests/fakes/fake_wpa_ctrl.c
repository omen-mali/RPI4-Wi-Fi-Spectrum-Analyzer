/**
 * @file fake_wpa_ctrl.c
 * @brief Fake libwpactrl (see fake_wpa_ctrl.h).
 */

#include "fake_wpa_ctrl.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNS   8
#define MAX_EVENTS  32
#define MAX_REPLIES 16
#define MAX_COUNTS  32
#define CMD_MAX     64

struct wpa_ctrl {
    int   in_use;
    int   attached;
    int   pipe_fd[2];            /* [0] read end, returned by get_fd */
    char *events[MAX_EVENTS];    /* FIFO */
    int   head, count;
};

typedef struct { char cmd[CMD_MAX]; int rc; char *reply; } reply_t;
typedef struct { char cmd[CMD_MAX]; int n; } count_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  calls_changed = PTHREAD_COND_INITIALIZER;

static struct wpa_ctrl conns[MAX_CONNS];
static reply_t replies[MAX_REPLIES];
static count_t counts[MAX_COUNTS];
static char   *scan_event;
static int     scan_event_set;   /* 0: use the default */
static int     break_next_pending;
static int     fail_opens_left;
static int     fail_open_at_k;   /* countdown; the open that takes it to 0 fails */
static int     open_now;

static const char *DEFAULT_SCAN_EVENT = "<3>CTRL-EVENT-SCAN-RESULTS ";

/* --- internals (lock held) ------------------------------------------------ */

static void queue_event(struct wpa_ctrl *c, const char *ev)
{
    if (c->count == MAX_EVENTS) {
        return;                  /* full: drop, like a lossy socket */
    }
    c->events[(c->head + c->count) % MAX_EVENTS] = strdup(ev);
    c->count++;
    char b = 'e';
    if (write(c->pipe_fd[1], &b, 1) != 1) {
        perror("fake_wpa_ctrl: pipe write");
    }
}

static void queue_event_all(const char *ev)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].in_use && conns[i].attached) {
            queue_event(&conns[i], ev);
        }
    }
}

static void close_conn(struct wpa_ctrl *c)
{
    for (int i = 0; i < c->count; i++) {
        free(c->events[(c->head + i) % MAX_EVENTS]);
    }
    close(c->pipe_fd[0]);
    close(c->pipe_fd[1]);
    memset(c, 0, sizeof(*c));
}

static void count_call(const char *cmd)
{
    for (int i = 0; i < MAX_COUNTS; i++) {
        if (counts[i].cmd[0] == '\0') {
            snprintf(counts[i].cmd, CMD_MAX, "%s", cmd);
        }
        if (strcmp(counts[i].cmd, cmd) == 0) {
            counts[i].n++;
            pthread_cond_broadcast(&calls_changed);
            return;
        }
    }
}

static int get_count(const char *cmd)
{
    for (int i = 0; i < MAX_COUNTS && counts[i].cmd[0]; i++) {
        if (strcmp(counts[i].cmd, cmd) == 0) {
            return counts[i].n;
        }
    }
    return 0;
}

/* --- scripting API ---------------------------------------------------------- */

void fake_wpa_reset(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].in_use) {
            close_conn(&conns[i]);
        }
    }
    for (int i = 0; i < MAX_REPLIES; i++) {
        free(replies[i].reply);
    }
    memset(replies, 0, sizeof(replies));
    memset(counts, 0, sizeof(counts));
    free(scan_event);
    scan_event = NULL;
    scan_event_set = 0;
    break_next_pending = 0;
    fail_opens_left = 0;
    fail_open_at_k = 0;
    open_now = 0;
    pthread_mutex_unlock(&lock);
}

void fake_wpa_set_reply(const char *cmd, int rc, const char *reply)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_REPLIES; i++) {
        if (replies[i].cmd[0] == '\0' || strcmp(replies[i].cmd, cmd) == 0) {
            snprintf(replies[i].cmd, CMD_MAX, "%s", cmd);
            free(replies[i].reply);
            replies[i].rc = rc;
            replies[i].reply = strdup(reply ? reply : "");
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

void fake_wpa_set_reply_file(const char *cmd, const char *fixture_name)
{
    char path[512];
    char buf[8192];
    snprintf(path, sizeof(path), "%s/scan/%s", FIXTURE_DIR, fixture_name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "fake_wpa_ctrl: cannot open fixture %s\n", path);
        abort();
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    fake_wpa_set_reply(cmd, 0, buf);
}

void fake_wpa_set_scan_event(const char *event_or_null)
{
    pthread_mutex_lock(&lock);
    free(scan_event);
    scan_event = event_or_null ? strdup(event_or_null) : NULL;
    scan_event_set = 1;
    pthread_mutex_unlock(&lock);
}

void fake_wpa_push_event(const char *event)
{
    pthread_mutex_lock(&lock);
    queue_event_all(event);
    pthread_mutex_unlock(&lock);
}

void fake_wpa_break_monitor(void)
{
    pthread_mutex_lock(&lock);
    break_next_pending = 1;
    pthread_mutex_unlock(&lock);
}

void fake_wpa_fail_opens(int n)
{
    pthread_mutex_lock(&lock);
    fail_opens_left = n;
    pthread_mutex_unlock(&lock);
}

void fake_wpa_fail_open_at(int k)
{
    pthread_mutex_lock(&lock);
    fail_open_at_k = k;
    pthread_mutex_unlock(&lock);
}

int fake_wpa_open_count(void)
{
    pthread_mutex_lock(&lock);
    int n = open_now;
    pthread_mutex_unlock(&lock);
    return n;
}

int fake_wpa_call_count(const char *cmd)
{
    pthread_mutex_lock(&lock);
    int n = get_count(cmd);
    pthread_mutex_unlock(&lock);
    return n;
}

int fake_wpa_wait_calls(const char *cmd, int n, int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&lock);
    int rc = 0;
    while (get_count(cmd) < n && rc == 0) {
        rc = pthread_cond_timedwait(&calls_changed, &lock, &deadline);
    }
    int ok = get_count(cmd) >= n;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

/* --- libwpactrl ------------------------------------------------------------- */

struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path)
{
    (void)ctrl_path;
    struct wpa_ctrl *c = NULL;
    pthread_mutex_lock(&lock);
    int fail = 0;
    if (fail_opens_left > 0) {
        fail_opens_left--;
        fail = 1;
    }
    if (fail_open_at_k > 0 && --fail_open_at_k == 0) {
        fail = 1;
    }
    for (int i = 0; !fail && i < MAX_CONNS; i++) {
        if (!conns[i].in_use) {
            if (pipe(conns[i].pipe_fd) != 0) {
                break;
            }
            conns[i].in_use = 1;
            c = &conns[i];
            open_now++;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    if (!c) {
        errno = ECONNREFUSED;
    }
    return c;
}

void wpa_ctrl_close(struct wpa_ctrl *ctrl)
{
    if (!ctrl) {
        return;
    }
    pthread_mutex_lock(&lock);
    if (ctrl->in_use) {
        close_conn(ctrl);
        open_now--;
    }
    pthread_mutex_unlock(&lock);
}

int wpa_ctrl_attach(struct wpa_ctrl *ctrl)
{
    pthread_mutex_lock(&lock);
    ctrl->attached = 1;
    pthread_mutex_unlock(&lock);
    return 0;
}

int wpa_ctrl_detach(struct wpa_ctrl *ctrl)
{
    pthread_mutex_lock(&lock);
    ctrl->attached = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}

int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len,
                     char *reply, size_t *reply_len,
                     void (*msg_cb)(char *msg, size_t len))
{
    (void)ctrl;
    (void)msg_cb;
    char name[CMD_MAX];
    snprintf(name, sizeof(name), "%.*s", (int)cmd_len, cmd);

    pthread_mutex_lock(&lock);
    int rc = 0;
    const char *text = "UNKNOWN COMMAND\n";
    for (int i = 0; i < MAX_REPLIES && replies[i].cmd[0]; i++) {
        if (strcmp(replies[i].cmd, name) == 0) {
            rc = replies[i].rc;
            text = replies[i].reply;
            break;
        }
    }
    if (rc == 0) {
        size_t n = strlen(text);
        if (n > *reply_len) {
            n = *reply_len;
        }
        memcpy(reply, text, n);            /* like the real one: no NUL */
        *reply_len = n;
        if (strcmp(name, "SCAN") == 0 &&
            (strncmp(text, "OK", 2) == 0 || strncmp(text, "FAIL-BUSY", 9) == 0)) {
            const char *ev = scan_event_set ? scan_event : DEFAULT_SCAN_EVENT;
            if (ev) {
                queue_event_all(ev);
            }
        }
    }
    count_call(name);
    pthread_mutex_unlock(&lock);
    return rc;
}

int wpa_ctrl_pending(struct wpa_ctrl *ctrl)
{
    pthread_mutex_lock(&lock);
    int rc;
    if (break_next_pending && ctrl->attached) {
        break_next_pending = 0;
        rc = -1;
    } else {
        rc = ctrl->count > 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len)
{
    pthread_mutex_lock(&lock);
    if (ctrl->count == 0) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    char *ev = ctrl->events[ctrl->head];
    ctrl->head = (ctrl->head + 1) % MAX_EVENTS;
    ctrl->count--;
    char b;
    if (read(ctrl->pipe_fd[0], &b, 1) != 1) {
        perror("fake_wpa_ctrl: pipe read");
    }
    pthread_mutex_unlock(&lock);

    size_t n = strlen(ev);
    if (n > *reply_len) {
        n = *reply_len;
    }
    memcpy(reply, ev, n);
    *reply_len = n;
    free(ev);
    return 0;
}

int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl)
{
    return ctrl ? ctrl->pipe_fd[0] : -1;
}
