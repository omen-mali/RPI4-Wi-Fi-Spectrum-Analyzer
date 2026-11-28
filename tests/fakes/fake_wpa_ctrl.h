/**
 * @file fake_wpa_ctrl.h
 * @brief Link-time fake of libwpactrl for the host tests, with scripting.
 *
 * Implements every wpa_ctrl_* function wifi_scanner.c declares. Replies are
 * scripted per command, events are queued per attached connection and
 * signalled through a pipe, so poll() on wpa_ctrl_get_fd() behaves as it
 * does on the real socket. All functions are thread-safe.
 */

#ifndef FAKE_WPA_CTRL_H
#define FAKE_WPA_CTRL_H

/** Forget all scripting and counters and close any connection left open. */
void fake_wpa_reset(void);

/**
 * Reply to the exact command @p cmd with rc (0, -1 or -2) and @p reply,
 * on every call until fake_wpa_reset(). Unscripted commands get rc 0 and
 * "UNKNOWN COMMAND\n".
 */
void fake_wpa_set_reply(const char *cmd, int rc, const char *reply);

/** As fake_wpa_set_reply(cmd, 0, <contents of FIXTURE_DIR/scan/name>). */
void fake_wpa_set_reply_file(const char *cmd, const char *fixture_name);

/**
 * Event queued on attached connections after each SCAN that replies "OK" or
 * "FAIL-BUSY". NULL: none. Default "<3>CTRL-EVENT-SCAN-RESULTS ".
 */
void fake_wpa_set_scan_event(const char *event_or_null);

/** The next wpa_ctrl_pending() on an attached connection returns -1. */
void fake_wpa_break_monitor(void);

/** Only the @p k-th wpa_ctrl_open() from now (1-based) returns NULL. */
void fake_wpa_fail_open_at(int k);

/** Connections currently open (opened minus closed). */
int fake_wpa_open_count(void);

/** How many times @p cmd has been requested since the last reset. */
int fake_wpa_call_count(const char *cmd);

/** Wait until @p cmd has been requested @p n times: 0, or -1 on timeout. */
int fake_wpa_wait_calls(const char *cmd, int n, int timeout_ms);

#endif /* FAKE_WPA_CTRL_H */
