/**
 * Windows 10 window for CW Skimmer.
 * Win32 (user32/gdi32), which ships with Windows. This file is not built
 * on Linux. The Makefile selects it when the host is Windows.
 *
 * The waterfall, the 1 kHz labels, and the copy on the right share one
 * row grid. A click keeps the green line on that row and tunes the radio
 * once. The picture center stays put when the dial report arrives.
 */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "cwskimmer_api.h"

#define WM_SPEC   (WM_APP + 1)
#define WM_DECODE (WM_APP + 2)
#define WM_STATS  (WM_APP + 3)

#define ID_START  1001
#define ID_STOP   1002
#define ID_CLEAR  1003
#define ID_STREAM 1004
#define ID_LAMP   1005
#define ID_SEND   1031
#define ID_WPM    1032
#define ID_CW     1030
#define ID_MORSE  1040
#define ID_SCROLL 1050
#define ID_FEDIT  1010
#define ID_FBUTTON 1020

#define MAX_BINS 2048
#define COL_CAP  400
#define SIDE_N   64
#define VIEW_SPAN 10000.0f

typedef struct {
    int hz;
    char text[160];
    float confidence;
} SideLine;

static cwskimmer_detector_t *g_det;
static pthread_t g_thread;
static int g_thread_live;
static HWND g_hwnd;
static HWND g_start, g_stop, g_clear, g_stream, g_lamp, g_scroll;
static HWND g_morse, g_cw, g_wpm, g_send;
static HWND g_fedit[5], g_fbtn[5];
static HFONT g_font;
static HBRUSH g_lamp_brush;
static CRITICAL_SECTION g_cs;

static float *g_ring;
static float *g_draw;
static int g_ring_nbin[COL_CAP];
static int g_draw_nbin[COL_CAP];
static float g_ring_noise[COL_CAP];
static float g_draw_noise[COL_CAP];
static int g_rw, g_rr, g_rn;
static int g_dw, g_dn;
static int g_bins;
static float g_bw;
static float g_reported;
static float g_label_center;
static int g_hold;
static double g_tune_hz;
static int g_audio_only;
static int g_connected;
static int g_running;
static int g_spec_posted;
static int g_view_set;
static int g_scroll_hz;
static int g_have_line;
static float g_line_hz;
static int g_selected_hz;
static SideLine g_side[SIDE_N];
static cwskimmer_decode_t g_dec_q[32];
static int g_dec_w, g_dec_r, g_dec_n;
static int g_dec_posted;
static char g_ini[MAX_PATH];
static RECT g_plot;
static RECT g_copy;
static RECT g_freq;

static float noise_of(const float *col, int n)
{
    float tmp[128];
    int step;
    int m = 0;
    int i, j;

    if (n <= 0) {
        return -100.0f;
    }
    step = n / 128;
    if (step < 1) {
        step = 1;
    }
    for (i = 0; i < n && m < 128; i += step) {
        tmp[m++] = col[i];
    }
    for (i = 1; i < m; i++) {
        float v = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    return tmp[(m * 2) / 5];
}

static int sign_of(void)
{
    return g_audio_only ? 1 : -1;
}

static void note_center(float reported)
{
    if (reported < 1.0e5f) {
        return;
    }
    g_reported = reported;
    if (g_label_center < 1.0e5f) {
        g_label_center = reported;
        return;
    }
    if (g_hold) {
        if (fabs((double)reported - g_tune_hz) < 40.0
            || fabs((double)reported - (double)g_label_center) < 40.0) {
            return;
        }
        g_hold = 0;
    }
    g_label_center = reported;
}

static void on_spectrum(const cwskimmer_spectrum_t *s, void *ud)
{
    int n;
    int post;
    (void)ud;
    if (!s || !s->power_spectrum || s->num_bins <= 1 || s->num_bins > MAX_BINS || !g_ring) {
        return;
    }
    n = s->num_bins;
    EnterCriticalSection(&g_cs);
    memcpy(g_ring + (size_t)g_rw * MAX_BINS, s->power_spectrum, (size_t)n * sizeof(float));
    g_ring_nbin[g_rw] = n;
    g_ring_noise[g_rw] = noise_of(s->power_spectrum, n);
    g_bins = n;
    if (s->bin_width > 0.0f) {
        g_bw = s->bin_width;
    }
    note_center(s->center_frequency);
    g_rw = (g_rw + 1) % COL_CAP;
    if (g_rn < COL_CAP) {
        g_rn++;
    } else {
        g_rr = (g_rr + 1) % COL_CAP;
    }
    post = !g_spec_posted;
    g_spec_posted = 1;
    LeaveCriticalSection(&g_cs);
    if (post && g_hwnd) {
        PostMessageA(g_hwnd, WM_SPEC, 0, 0);
    }
}

static void on_decode(const cwskimmer_decode_t *d, void *ud)
{
    int post;
    (void)ud;
    if (!d) {
        return;
    }
    EnterCriticalSection(&g_cs);
    g_dec_q[g_dec_w] = *d;
    g_dec_w = (g_dec_w + 1) % 32;
    if (g_dec_n < 32) {
        g_dec_n++;
    } else {
        g_dec_r = (g_dec_r + 1) % 32;
    }
    post = !g_dec_posted;
    g_dec_posted = 1;
    LeaveCriticalSection(&g_cs);
    if (post && g_hwnd) {
        PostMessageA(g_hwnd, WM_DECODE, 0, 0);
    }
}

static void on_stats(const cwskimmer_stats_t *st, void *ud)
{
    (void)ud;
    if (!st || !g_hwnd) {
        return;
    }
    PostMessageA(g_hwnd, WM_STATS, (WPARAM)st->connected, (LPARAM)st->audio_only);
}

static void *det_thread(void *p)
{
    cwskimmer_start((cwskimmer_detector_t *)p);
    return NULL;
}

static void ini_path(void)
{
    char *slash;
    GetModuleFileNameA(NULL, g_ini, MAX_PATH);
    slash = strrchr(g_ini, '\\');
    if (!slash) {
        strcpy(g_ini, "cw-skimmer-fn.ini");
        return;
    }
    strcpy(slash + 1, "cw-skimmer-fn.ini");
}

static void load_fn(void)
{
    int i;
    for (i = 0; i < 5; i++) {
        char key[8], text[160];
        sprintf(key, "F%d", i + 1);
        text[0] = '\0';
        GetPrivateProfileStringA("functionKeys", key, "", text, sizeof text, g_ini);
        if (g_fedit[i]) {
            SetWindowTextA(g_fedit[i], text);
        }
    }
}

static void save_fn(void)
{
    int i;
    for (i = 0; i < 5; i++) {
        char key[8], text[160];
        sprintf(key, "F%d", i + 1);
        text[0] = '\0';
        if (g_fedit[i]) {
            GetWindowTextA(g_fedit[i], text, sizeof text);
        }
        WritePrivateProfileStringA("functionKeys", key, text, g_ini);
    }
}

static const char *config_path(void)
{
    static char path[MAX_PATH];
    char *slash;
    GetModuleFileNameA(NULL, path, MAX_PATH);
    slash = strrchr(path, '\\');
    if (slash) {
        strcpy(slash + 1, "cw-skimmer.conf");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    return "cw-skimmer.conf";
}

static void set_status(const char *text)
{
    char buf[256];
    if (g_have_line && g_label_center > 1.0e5f) {
        double rf = (double)g_label_center + (double)sign_of() * (double)g_line_hz;
        snprintf(buf, sizeof buf, "%s    RF %.6f MHz", text, rf / 1.0e6);
    } else {
        snprintf(buf, sizeof buf, "%s", text);
    }
    SetWindowTextA(g_hwnd, buf);
}

static void lamp(void)
{
    int on = g_running && g_connected;
    SetWindowTextA(g_lamp, on ? "Connected" : "Not connected");
    if (g_lamp_brush) {
        DeleteObject(g_lamp_brush);
    }
    g_lamp_brush = CreateSolidBrush(on ? RGB(27, 94, 32) : RGB(74, 21, 21));
    InvalidateRect(g_lamp, NULL, TRUE);
}

static void view_limits(float *full_low, float *view_span, float *max_scroll)
{
    float span = (g_bins > 1 && g_bw > 0.0f) ? (float)g_bins * g_bw : 48000.0f;
    *view_span = (span > VIEW_SPAN + 1.0f) ? VIEW_SPAN : span;
    *full_low = -0.5f * span;
    *max_scroll = span - *view_span;
    if (*max_scroll < 0.0f) {
        *max_scroll = 0.0f;
    }
}

static void sync_scroll(void)
{
    float full_low, view_span, max_scroll;
    SCROLLINFO si;
    view_limits(&full_low, &view_span, &max_scroll);
    if (!g_view_set) {
        float max_low = full_low + max_scroll;
        float view_low = -0.5f * view_span;
        if (view_low < full_low) {
            view_low = full_low;
        }
        if (view_low > max_low) {
            view_low = max_low;
        }
        g_scroll_hz = (int)(max_low - view_low);
        g_view_set = 1;
    }
    if (g_scroll_hz < 0) {
        g_scroll_hz = 0;
    }
    if ((float)g_scroll_hz > max_scroll) {
        g_scroll_hz = (int)max_scroll;
    }
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)max_scroll;
    si.nPage = (UINT)view_span;
    si.nPos = g_scroll_hz;
    SetScrollInfo(g_scroll, SB_CTL, &si, TRUE);
}

static void view_edges(float *low, float *high)
{
    float full_low, view_span, max_scroll, max_low;
    view_limits(&full_low, &view_span, &max_scroll);
    max_low = full_low + max_scroll;
    *low = max_low - (float)g_scroll_hz;
    *high = *low + view_span;
}

static int y_for_hz(float hz, float high, float span, int top, int height)
{
    float frac;
    if (span < 1.0f || height <= 0) {
        return top;
    }
    frac = (high - hz) / span;
    return top + (int)lroundf(frac * (float)height);
}

static float hz_for_y(int y, float high, float span, int top, int height)
{
    float frac;
    if (height <= 0) {
        return 0.0f;
    }
    frac = (float)(y - top) / (float)height;
    if (frac < 0.0f) {
        frac = 0.0f;
    }
    if (frac > 1.0f) {
        frac = 1.0f;
    }
    return high - frac * span;
}

static SideLine *side_for(int hz, int create)
{
    int i;
    int free_i = -1;
    for (i = 0; i < SIDE_N; i++) {
        if (g_side[i].hz == hz) {
            return &g_side[i];
        }
        if (free_i < 0 && g_side[i].hz == INT_MIN) {
            free_i = i;
        }
    }
    if (!create || free_i < 0) {
        return NULL;
    }
    g_side[free_i].hz = hz;
    g_side[free_i].text[0] = '\0';
    g_side[free_i].confidence = 0.0f;
    return &g_side[free_i];
}

static void apply_decode(const cwskimmer_decode_t *d)
{
    int snapped;
    SideLine *line;
    if (!d->text[0] && !d->new_letter) {
        return;
    }
    snapped = (int)floorf(d->freq_offset_hz / 1000.0f) * 1000;
    line = side_for(snapped, 1);
    if (!line) {
        return;
    }
    if (d->text[0]) {
        strncpy(line->text, d->text, sizeof line->text - 1);
        line->text[sizeof line->text - 1] = '\0';
    }
    line->confidence = d->text_confidence;
    if (g_have_line && snapped == g_selected_hz && line->text[0]) {
        SetWindowTextA(g_morse, line->text);
    }
}

static void drain_spec(void)
{
    EnterCriticalSection(&g_cs);
    while (g_rn > 0) {
        int n = g_ring_nbin[g_rr];
        if (n > MAX_BINS) {
            n = MAX_BINS;
        }
        memcpy(g_draw + (size_t)g_dw * MAX_BINS, g_ring + (size_t)g_rr * MAX_BINS,
               (size_t)n * sizeof(float));
        g_draw_nbin[g_dw] = n;
        g_draw_noise[g_dw] = g_ring_noise[g_rr];
        g_dw = (g_dw + 1) % COL_CAP;
        if (g_dn < COL_CAP) {
            g_dn++;
        }
        g_rr = (g_rr + 1) % COL_CAP;
        g_rn--;
    }
    g_spec_posted = 0;
    LeaveCriticalSection(&g_cs);
    sync_scroll();
}

static void drain_decode(void)
{
    EnterCriticalSection(&g_cs);
    while (g_dec_n > 0) {
        cwskimmer_decode_t d = g_dec_q[g_dec_r];
        g_dec_r = (g_dec_r + 1) % 32;
        g_dec_n--;
        LeaveCriticalSection(&g_cs);
        apply_decode(&d);
        EnterCriticalSection(&g_cs);
    }
    g_dec_posted = 0;
    LeaveCriticalSection(&g_cs);
}

static COLORREF power_color(float power, float noise)
{
    float t;
    int v;
    float floor = noise > -110.0f ? noise : -110.0f;
    float threshold = floor + 8.0f;
    if (power < threshold) {
        return RGB(0, 0, 0);
    }
    t = (power - threshold) / 35.0f;
    if (t < 0.0f) {
        t = 0.0f;
    }
    if (t > 1.0f) {
        t = 1.0f;
    }
    t = powf(t, 0.85f);
    v = (int)(t * 255.0f + 0.5f);
    return RGB(v, v, v);
}

static void paint_spectrum(HDC dc)
{
    float low, high, span;
    int plot_h, plot_w, copy_w, i, y;
    RECT plot = g_plot;
    HDC mem;
    HBITMAP bmp, old;
    HFONT old_font;
    int last_y = -10000;
    int row_h;

    view_edges(&low, &high);
    span = high - low;
    if (span < 1.0f) {
        span = 1.0f;
    }
    plot_h = plot.bottom - plot.top;
    plot_w = plot.right - plot.left;
    copy_w = g_copy.right - g_copy.left;
    if (plot_h <= 1 || plot_w <= 1) {
        return;
    }

    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, plot_w + (g_freq.right - g_freq.left) + copy_w,
                                 plot_h);
    old = (HBITMAP)SelectObject(mem, bmp);
    old_font = (HFONT)SelectObject(mem, g_font);
    {
        RECT all = {0, 0, plot_w + (g_freq.right - g_freq.left) + copy_w, plot_h};
        FillRect(mem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    SetBkMode(mem, TRANSPARENT);
    row_h = 16;
    {
        TEXTMETRIC tm;
        if (GetTextMetrics(mem, &tm)) {
            row_h = tm.tmHeight;
        }
    }

    for (y = 0; y < plot_h; y++) {
        float hz = high - ((float)y / (float)plot_h) * span;
        int bin;
        int col;
        if (g_bins <= 1 || g_bw <= 0.0f) {
            break;
        }
        bin = g_bins / 2 + (int)lroundf(hz / g_bw);
        for (col = 0; col < plot_w; col++) {
            int idx;
            int src;
            float power = -200.0f;
            float noise = -100.0f;
            COLORREF c = RGB(0, 0, 0);
            if (g_dn > 0) {
                int visible = g_dn < plot_w ? g_dn : plot_w;
                int first = g_dn - visible;
                src = col - (plot_w - visible);
                if (src >= 0) {
                    idx = (g_dw - g_dn + first + src) % COL_CAP;
                    if (idx < 0) {
                        idx += COL_CAP;
                    }
                    if (bin >= 0 && bin < g_draw_nbin[idx]) {
                        power = g_draw[(size_t)idx * MAX_BINS + bin];
                        noise = g_draw_noise[idx];
                        c = power_color(power, noise);
                    }
                }
            }
            SetPixel(mem, col, y, c);
        }
    }

    if (g_have_line) {
        int ly = y_for_hz(g_line_hz, high, span, 0, plot_h);
        HPEN pen = CreatePen(PS_DASH, 1, RGB(0, 255, 120));
        HPEN old_pen = (HPEN)SelectObject(mem, pen);
        if (ly >= 0 && ly < plot_h) {
            MoveToEx(mem, 0, ly, NULL);
            LineTo(mem, plot_w, ly);
        }
        SelectObject(mem, old_pen);
        DeleteObject(pen);
    }

    SetTextColor(mem, RGB(242, 242, 242));
    {
        float first = ceilf(low / 1000.0f) * 1000.0f;
        float hz;
        int freq_w = g_freq.right - g_freq.left;
        for (hz = first; hz <= high + 0.5f; hz += 1000.0f) {
            int ly = y_for_hz(hz, high, span, 0, plot_h);
            double rf;
            double mhz;
            char label[32];
            RECT lr;
            SideLine *side;
            RECT cr;
            if (ly < 0 || ly > plot_h) {
                continue;
            }
            if (last_y >= 0 && abs(ly - last_y) < row_h + 1) {
                continue;
            }
            last_y = ly;
            rf = (double)g_label_center + (double)sign_of() * (double)hz;
            mhz = round(rf / 1000.0) / 1000.0;
            snprintf(label, sizeof label, "%.3f", mhz);
            lr.left = plot_w;
            lr.right = plot_w + freq_w;
            lr.top = ly - row_h / 2;
            lr.bottom = lr.top + row_h;
            DrawTextA(mem, label, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            side = side_for((int)hz, 0);
            if (side && side->text[0]) {
                cr.left = plot_w + freq_w;
                cr.right = cr.left + copy_w;
                cr.top = lr.top;
                cr.bottom = lr.bottom;
                DrawTextA(mem, side->text, -1, &cr,
                          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }
    }

    BitBlt(dc, plot.left, plot.top, plot_w, plot_h, mem, 0, 0, SRCCOPY);
    BitBlt(dc, g_freq.left, g_freq.top, g_freq.right - g_freq.left, plot_h, mem, plot_w, 0, SRCCOPY);
    BitBlt(dc, g_copy.left, g_copy.top, copy_w, plot_h, mem, plot_w + (g_freq.right - g_freq.left), 0, SRCCOPY);
    SelectObject(mem, old_font);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void on_click(int y)
{
    float low, high, span, hz;
    double rf;
    int plot_h;
    if (g_plot.bottom <= g_plot.top) {
        return;
    }
    view_edges(&low, &high);
    span = high - low;
    plot_h = g_plot.bottom - g_plot.top;
    hz = hz_for_y(y, high, span, g_plot.top, plot_h);
    g_line_hz = hz;
    g_have_line = 1;
    g_selected_hz = (int)floorf(hz / 1000.0f) * 1000;
    {
        SideLine *side = side_for(g_selected_hz, 0);
        SetWindowTextA(g_morse, (side && side->text[0]) ? side->text : "");
    }
    if (g_label_center > 1.0e5f && g_det) {
        rf = (double)g_label_center + (double)sign_of() * (double)hz;
        if (fabs(hz) >= 30.0f) {
            g_tune_hz = rf;
            g_hold = 1;
            cwskimmer_tune_hz(g_det, (long long)llround(rf));
        }
    }
    set_status(g_running ? "RUNNING" : "STOPPED");
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static int blank_text(const char *s)
{
    if (!s) {
        return 1;
    }
    while (*s) {
        if (*s != ' ' && *s != '\t') {
            return 0;
        }
        s++;
    }
    return 1;
}

static void send_text(const char *text)
{
    if (blank_text(text)) {
        return;
    }
    if (!g_running || !g_det) {
        set_status("Start the radio to send CW");
        return;
    }
    cwskimmer_cw_send(g_det, text);
}

static void send_fn(int index)
{
    char text[160];
    if (index < 0 || index > 4 || !g_fedit[index]) {
        return;
    }
    GetWindowTextA(g_fedit[index], text, sizeof text);
    save_fn();
    send_text(text);
}

static void start_radio(void)
{
    char wpm[16];
    if (!g_det || g_running) {
        return;
    }
    cwskimmer_config_set(g_det, "training_file", "");
    cwskimmer_config_set(g_det, "spectrum_span_hz", "0");
    cwskimmer_config_set(g_det, "tci_stream_mode", g_audio_only ? "audio" : "iq");
    GetWindowTextA(g_wpm, wpm, sizeof wpm);
    cwskimmer_cw_set_wpm(g_det, atoi(wpm) > 0 ? atoi(wpm) : 20);
    if (pthread_create(&g_thread, NULL, det_thread, g_det) != 0) {
        set_status("Could not start");
        return;
    }
    g_thread_live = 1;
    g_running = 1;
    lamp();
    set_status("RUNNING");
}

static void stop_radio(void)
{
    if (g_det) {
        cwskimmer_stop(g_det);
    }
    if (g_thread_live) {
        pthread_join(g_thread, NULL);
        g_thread_live = 0;
    }
    g_running = 0;
    g_connected = 0;
    lamp();
    set_status("STOPPED");
}

static void layout(HWND hwnd)
{
    RECT rc;
    int w, h, top, freq_w, copy_w, scroll_w, plot_w;
    int i, x;
    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;
    top = 4;
    MoveWindow(g_start, 8, top, 72, 26, TRUE);
    MoveWindow(g_stop, 84, top, 72, 26, TRUE);
    MoveWindow(g_lamp, 164, top, 140, 26, TRUE);
    MoveWindow(g_clear, 312, top, 72, 26, TRUE);
    MoveWindow(g_stream, 390, top, 110, 26, TRUE);
    x = 8;
    for (i = 0; i < 5; i++) {
        MoveWindow(g_fbtn[i], x, 36, 36, 24, TRUE);
        MoveWindow(g_fedit[i], x + 38, 36, 90, 24, TRUE);
        x += 134;
    }
    MoveWindow(g_cw, x, 36, 180, 24, TRUE);
    MoveWindow(g_send, x + 184, 36, 64, 24, TRUE);
    MoveWindow(g_wpm, x + 254, 36, 48, 24, TRUE);

    freq_w = 88;
    copy_w = 280;
    scroll_w = 18;
    g_freq.left = 4;
    g_freq.top = 68;
    g_freq.right = g_freq.left + freq_w;
    g_plot.left = g_freq.right + 4;
    g_plot.top = 68;
    g_plot.right = w - copy_w - scroll_w - 12;
    if (g_plot.right < g_plot.left + 40) {
        g_plot.right = g_plot.left + 40;
    }
    g_copy.left = g_plot.right + 4;
    g_copy.top = 68;
    g_copy.right = g_copy.left + copy_w;
    plot_w = g_plot.right - g_plot.left;
    (void)plot_w;
    g_plot.bottom = h - 120;
    g_freq.bottom = g_plot.bottom;
    g_copy.bottom = g_plot.bottom;
    if (g_plot.bottom < g_plot.top + 40) {
        g_plot.bottom = g_plot.top + 40;
    }
    MoveWindow(g_scroll, g_copy.right + 4, g_plot.top, scroll_w, g_plot.bottom - g_plot.top, TRUE);
    MoveWindow(g_morse, 8, g_plot.bottom + 8, w - 16, 80, TRUE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_font = CreateFontA(-MulDiv(11, 96, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
        g_start = CreateWindowA("BUTTON", "Start", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)ID_START, NULL, NULL);
        g_stop = CreateWindowA("BUTTON", "Stop", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)ID_STOP, NULL, NULL);
        g_lamp = CreateWindowA("STATIC", "Not connected", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 10, 10, hwnd, (HMENU)ID_LAMP, NULL, NULL);
        g_clear = CreateWindowA("BUTTON", "Clear", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)ID_CLEAR, NULL, NULL);
        g_stream = CreateWindowA("BUTTON", "Stream: IQ", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)ID_STREAM, NULL, NULL);
        {
            int i;
            for (i = 0; i < 5; i++) {
                char name[8];
                sprintf(name, "F%d", i + 1);
                g_fbtn[i] = CreateWindowA("BUTTON", name, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)(ID_FBUTTON + i), NULL, NULL);
                g_fedit[i] = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, (HMENU)(UINT_PTR)(ID_FEDIT + i), NULL, NULL);
                SendMessageA(g_fedit[i], WM_SETFONT, (WPARAM)g_font, TRUE);
            }
        }
        g_cw = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, (HMENU)ID_CW, NULL, NULL);
        g_send = CreateWindowA("BUTTON", "Send", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, (HMENU)ID_SEND, NULL, NULL);
        g_wpm = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "20", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 10, 10, hwnd, (HMENU)ID_WPM, NULL, NULL);
        g_morse = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 0, 0, 10, 10, hwnd, (HMENU)ID_MORSE, NULL, NULL);
        g_scroll = CreateWindowA("SCROLLBAR", "", WS_CHILD | WS_VISIBLE | SBS_VERT, 0, 0, 10, 10, hwnd, (HMENU)ID_SCROLL, NULL, NULL);
        SendMessageA(g_cw, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageA(g_wpm, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageA(g_morse, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageA(g_lamp, WM_SETFONT, (WPARAM)g_font, TRUE);
        ini_path();
        load_fn();
        lamp();
        layout(hwnd);
        return 0;
    case WM_SIZE:
        layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_SPEC:
        drain_spec();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DECODE:
        drain_decode();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_STATS:
        g_connected = (int)wp;
        g_audio_only = (int)lp;
        SetWindowTextA(g_stream, g_audio_only ? "Stream: Audio" : "Stream: IQ");
        lamp();
        set_status(g_running ? "RUNNING" : "STOPPED");
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_START:
            start_radio();
            break;
        case ID_STOP:
            stop_radio();
            break;
        case ID_CLEAR:
            {
                int i;
                for (i = 0; i < SIDE_N; i++) {
                    g_side[i].hz = INT_MIN;
                    g_side[i].text[0] = '\0';
                }
                SetWindowTextA(g_morse, "");
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        case ID_STREAM:
            g_audio_only = !g_audio_only;
            if (g_det) {
                cwskimmer_config_set(g_det, "tci_stream_mode", g_audio_only ? "audio" : "iq");
            }
            SetWindowTextA(g_stream, g_audio_only ? "Stream: Audio" : "Stream: IQ");
            break;
        case ID_SEND:
            {
                char text[256];
                GetWindowTextA(g_cw, text, sizeof text);
                send_text(text);
            }
            break;
        default:
            if (LOWORD(wp) >= ID_FBUTTON && LOWORD(wp) < ID_FBUTTON + 5) {
                send_fn(LOWORD(wp) - ID_FBUTTON);
            }
            break;
        }
        return 0;
    case WM_VSCROLL:
        if ((HWND)lp == g_scroll) {
            float full_low, view_span, max_scroll;
            SCROLLINFO si;
            view_limits(&full_low, &view_span, &max_scroll);
            switch (LOWORD(wp)) {
            case SB_LINEUP: g_scroll_hz -= 1000; break;
            case SB_LINEDOWN: g_scroll_hz += 1000; break;
            case SB_PAGEUP: g_scroll_hz -= (int)view_span; break;
            case SB_PAGEDOWN: g_scroll_hz += (int)view_span; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
                memset(&si, 0, sizeof si);
                si.cbSize = sizeof si;
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(g_scroll, SB_CTL, &si);
                g_scroll_hz = si.nTrackPos;
                break;
            default:
                break;
            }
            g_view_set = 1;
            sync_scroll();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        g_scroll_hz -= (GET_WHEEL_DELTA_WPARAM(wp) / 120) * 1000;
        g_view_set = 1;
        sync_scroll();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (GET_X_LPARAM(lp) >= g_plot.left && GET_X_LPARAM(lp) < g_plot.right
            && GET_Y_LPARAM(lp) >= g_plot.top && GET_Y_LPARAM(lp) < g_plot.bottom) {
            on_click(GET_Y_LPARAM(lp));
        }
        return 0;
    case WM_KEYDOWN:
        if (wp >= VK_F1 && wp <= VK_F5) {
            send_fn((int)wp - VK_F1);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_lamp) {
            int on = g_running && g_connected;
            SetTextColor((HDC)wp, on ? RGB(232, 245, 233) : RGB(255, 205, 210));
            SetBkColor((HDC)wp, on ? RGB(27, 94, 32) : RGB(74, 21, 21));
            return (LRESULT)g_lamp_brush;
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paint_spectrum(dc);
            EndPaint(hwnd, &ps);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        save_fn();
        stop_radio();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    int i;
    WSADATA wsa;
    (void)prev;
    (void)cmd;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&g_cs);
    g_ring = (float *)calloc((size_t)COL_CAP * MAX_BINS, sizeof(float));
    g_draw = (float *)calloc((size_t)COL_CAP * MAX_BINS, sizeof(float));
    for (i = 0; i < SIDE_N; i++) {
        g_side[i].hz = INT_MIN;
    }
    g_det = cwskimmer_detector_create(config_path());
    if (g_det) {
        cwskimmer_set_spectrum_callback(g_det, on_spectrum, NULL);
        cwskimmer_set_decode_callback(g_det, on_decode, NULL);
        cwskimmer_set_stats_callback(g_det, on_stats, NULL);
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "CWSkimmerWin";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("CWSkimmerWin", "CW Skimmer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1200, 820,
                           NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        return 1;
    }
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    set_status(g_det ? "STOPPED" : "No detector");
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam >= VK_F1 && msg.wParam <= VK_F5) {
            send_fn((int)msg.wParam - VK_F1);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (g_det) {
        cwskimmer_detector_destroy(g_det);
        g_det = NULL;
    }
    if (g_font) {
        DeleteObject(g_font);
    }
    if (g_lamp_brush) {
        DeleteObject(g_lamp_brush);
    }
    free(g_ring);
    free(g_draw);
    DeleteCriticalSection(&g_cs);
    WSACleanup();
    return 0;
}
