/*
 * main.c - u60pro-devui entry point.
 *
 * Wires the clean-room DRM display and evdev touch backends into LVGL, then
 * runs the GUI event loop. No vendor libraries are linked.
 *
 * SPDX-License-Identifier: MIT
 */
#include "backlight.h"
#include "data.h"
#include "drm_disp.h"
#include "touch_input.h"
#include "devui_config.h"
#include "ui.h"
#include "lvgl.h"

#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static drm_disp_t    g_disp;
static touch_input_t g_touch;
unsigned long        g_frame_count = 0;   /* flushes pushed to the panel */
static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig) { (void)sig; g_run = 0; }

/* Manual screen-capture hook for debugging: `touch /tmp/u60-dumpfb` on the
 * device dumps the current scanout buffer as raw RGB565 to /tmp/fb.dump.
 * Mirrors htmlmain.c's maybe_dump_fb() so the same pull-and-decode tooling
 * works against either renderer. */
static void maybe_dump_fb(drm_disp_t *d)
{
    if (access("/tmp/u60-dumpfb", F_OK) != 0) return;
    FILE *f = fopen("/tmp/fb.dump", "wb");
    if (!f) return;
    for (int y = 0; y < d->height; y++)
        fwrite(&d->fb[(size_t)y * d->pitch_px], sizeof(uint16_t), d->width, f);
    fclose(f);
}

/* Monotonic millisecond tick source for LVGL. */
static uint32_t millis_cb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

/* Copy an LVGL-rendered area into the DRM framebuffer (RGB565), applying the
 * panel's 180° mounting rotation, then mark the region dirty. */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src = (const uint16_t *)px_map;
    const int aw       = area->x2 - area->x1 + 1;
    const int W        = g_disp.width;
    const int H        = g_disp.height;
    const int pitch_px = g_disp.pitch_px;

    int dx1, dy1, dx2, dy2;

#if DEVUI_ROTATE_180
    /* 180°: dest row decreases, and within a row dest x decreases — walk both
     * with pointer arithmetic (no per-pixel multiplies). */
    for (int y = area->y1; y <= area->y2; y++) {
        const uint16_t *sp = src + (size_t)(y - area->y1) * aw;
        uint16_t *dp = g_disp.fb + (size_t)((H - 1) - y) * pitch_px + ((W - 1) - area->x1);
        for (int i = 0; i < aw; i++) *dp-- = *sp++;
    }
    dx1 = (W - 1) - area->x2; dx2 = (W - 1) - area->x1;
    dy1 = (H - 1) - area->y2; dy2 = (H - 1) - area->y1;
#else
    /* No rotation: straight row memcpy. */
    for (int y = area->y1; y <= area->y2; y++) {
        const uint16_t *sp = src + (size_t)(y - area->y1) * aw;
        uint16_t *dp = g_disp.fb + (size_t)y * pitch_px + area->x1;
        memcpy(dp, sp, (size_t)aw * sizeof(uint16_t));
    }
    dx1 = area->x1; dx2 = area->x2;
    dy1 = area->y1; dy2 = area->y2;
#endif
    drm_disp_dirty(&g_disp, dx1, dy1, dx2, dy2);
    g_frame_count++;

    lv_display_flush_ready(disp);
}

/* U60_DEVUI_TAPLOG=1: log what each press lands on (diagnosing taps that
 * "go nowhere" on the device, which the offscreen test cannot see). */
static void taplog_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_event_get_param(e) ? (lv_indev_t *)lv_event_get_param(e) : lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    lv_obj_t *o = in ? lv_indev_get_active_obj() : NULL;
    lv_area_t a = { 0, 0, 0, 0 };
    if (o) lv_obj_get_coords(o, &a);
    fprintf(stderr, "tap: %s at %d,%d -> obj %p [%d,%d %d,%d]\n",
            lv_event_get_code(e) == LV_EVENT_PRESSED ? "press" : "click",
            (int)p.x, (int)p.y, (void *)o, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) ui_debug_tap();
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    int x, y, pressed;
    touch_input_read(&g_touch, &x, &y, &pressed);
    /* A quick tap that fell entirely inside one read (the loop was busy with
     * a slow poll) would reach LVGL as nothing at all: report it as a press
     * now and its release on the next read. */
    static int replay_up, rx, ry;
    if (replay_up) {
        replay_up = 0;
        if (!pressed) { x = rx; y = ry; }
    } else if (!pressed && touch_input_take_lost_tap(&g_touch, &rx, &ry)) {
        x = rx; y = ry; pressed = 1; replay_up = 1;
    }
    {
        static int was, logit = -1;
        if (logit < 0) logit = getenv("U60_DEVUI_TAPLOG") != NULL;
        if (logit && pressed != was)
            fprintf(stderr, "tap: indev %s at %d,%d (raw %d,%d)\n", pressed ? "down" : "up", x, y,
                    g_touch.raw_cur_x, g_touch.raw_cur_y);
        was = pressed;
    }
    /* A dark screen takes no taps (ui_touch_filter). If a press that LVGL
     * already has is cut off (screen turned off under the finger), cancel it
     * instead of releasing it, or the release would count as a click. */
    static int fwd_was;
    int fwd = ui_touch_filter(pressed, x, y);
    if (fwd_was && !fwd && pressed) lv_indev_reset(indev, NULL);
    fwd_was = fwd;
    data->point.x = x;
    data->point.y = y;
    data->state = fwd ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (drm_disp_init(&g_disp) != 0) {
        fprintf(stderr, "fatal: display init failed\n");
        return 1;
    }
    /* Touch is optional: the UI still renders without it. */
    if (touch_input_init(&g_touch, g_disp.width, g_disp.height) != 0)
        fprintf(stderr, "warning: continuing without touch input\n");

    lv_init();
    lv_tick_set_cb(millis_cb);

    lv_display_t *disp = lv_display_create(g_disp.width, g_disp.height);
    lv_display_set_flush_cb(disp, disp_flush_cb);

    /* Full-screen double buffers: a full redraw (e.g. a page swipe) becomes a
     * single flush + one DIRTYFB instead of many small ones. */
    static uint16_t buf1[DEVUI_FALLBACK_WIDTH * DEVUI_FALLBACK_HEIGHT];
    static uint16_t buf2[DEVUI_FALLBACK_WIDTH * DEVUI_FALLBACK_HEIGHT];
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_read_cb);
    /* Damp the fling. LVGL's default throw (10 = 10% decay per frame) gives
     * a long glide, and with 6 snap points on a 320px panel a quick flick
     * lands several pages past the one you aimed at — seen repeatedly with
     * injected swipes (a 220px/300ms swipe from 首页 landed on 性能测试;
     * a slow 150px/900ms drag moves exactly one page). Synthetic injection
     * can bunch events and exaggerate the measured velocity, so treat the
     * multi-page number as indicative, not exact — but the old litehtml
     * renderer had no inertia at all (one swipe = one page, always), which
     * is the behaviour this device's user is comparing against. 40 keeps a
     * deliberate drag smooth while stopping the glide quickly. */
    lv_indev_set_scroll_throw(indev, 40);
    if (getenv("U60_DEVUI_TAPLOG")) {
        lv_indev_add_event_cb(indev, taplog_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, taplog_cb, LV_EVENT_CLICKED, NULL);
    }

    ui_set_launch(argc, argv);
    ui_create();

    /*
     * Screen-off idle. With the panel lit the loop wakes every ≤ 8 ms so
     * animations and touch stay smooth. Dark, that same loop woke ~125 times a
     * second for nothing (measured 2026-09-23), keeping the CPU out of its
     * deep idle states. So while dark, the fast LVGL timers are paused
     * (display refresh ~33 ms, touch read ~30 ms, the UI's 50 ms key poll) and
     * the loop sleeps in poll() on the touch device, the power key and the
     * data-service socket, at most until the next timer that still runs (the
     * 1 s refresh). Input resumes the timers at once and the normal code
     * decides what it means (double-tap wake via ui_touch_filter, power key);
     * if the screen is still dark afterwards the loop goes back to sleep.
     * Rendering waits while dark; invalidated areas are drawn on wake.
     * The process stays alive throughout (u60-uid only checks it is there).
     */
    lv_timer_t *refr_timer = lv_display_get_refr_timer(disp);
    lv_timer_t *read_timer = lv_indev_get_read_timer(indev);
    int dark = 0;

    while (g_run) {
        uint32_t idle;

        /* Drive the zwrt-datad transport. data_refresh() only *copies* the
         * UI-visible snapshot; the snapshot itself only advances when
         * something drains the SSE stream (data_backend_poll) and promotes
         * the result (data_backend_commit_latest). htmlmain.c does this in
         * its own loop (htmlmain.c:5656/5717) — the LVGL loop never did, so
         * every page showed whatever the backend happened to report at
         * startup and then froze: uptime, CPU, throughput, signal all stuck,
         * and the charts drew 40 identical points. Found because a speed
         * test moved nothing on screen. */
        if (data_backend_poll(millis_cb()))
            (void)data_backend_commit_latest();

        idle = lv_timer_handler();
        maybe_dump_fb(&g_disp);

        int want_dark = !backlight_is_on();
        if (want_dark != dark) {
            dark = want_dark;
            if (refr_timer) { if (dark) lv_timer_pause(refr_timer); else lv_timer_resume(refr_timer); }
            if (read_timer) { if (dark) lv_timer_pause(read_timer); else lv_timer_resume(read_timer); }
            ui_idle(dark);
        }
        if (dark) {
            struct pollfd pf[3];
            int n = 0;
            if (g_touch.fd >= 0)        { pf[n].fd = g_touch.fd;        pf[n].events = POLLIN; n++; }
            if (ui_key_fd() >= 0)       { pf[n].fd = ui_key_fd();       pf[n].events = POLLIN; n++; }
            int input_n = n;
            if (data_backend_fd() >= 0) { pf[n].fd = data_backend_fd(); pf[n].events = POLLIN; n++; }
            int wait_ms = idle > 1000 ? 1000 : (int)idle;
            if (poll(pf, (nfds_t)n, wait_ms) > 0) {
                for (int i = 0; i < input_n; i++) {
                    if (pf[i].revents) {
                        /* touch or key: let LVGL and key_poll_cb handle it now */
                        dark = 0;
                        if (refr_timer) lv_timer_resume(refr_timer);
                        if (read_timer) lv_timer_resume(read_timer);
                        ui_idle(0);
                        break;
                    }
                }
            }
            continue;
        }
        if (idle > 8) idle = 8;         /* render promptly during animations */
        usleep((useconds_t)idle * 1000);
    }

    data_set_pace(1);   /* don't leave datad slow behind a stopped UI */
    drm_disp_close(&g_disp);
    touch_input_close(&g_touch);
    return 0;
}
