// Harness only: a fake RTL-SDR dongle that plays a .cu8 recording ($IQFILE).
// IQPACE=1 -> in real time (as the phone gets it), else as fast as possible.
// IQLOOP=1 -> at the end of the file start again from the top, for ever (a ten-second
// recording becomes ten minutes of listening; the seam is a click, like a retune without one).
// RTLTRACE=calls.txt -> write every driver call the app makes, with its arguments, to that file
// (build 17: the proof that the source split changed nothing the dongle sees - build 16 and
// build 17 must write the same file; sample blocks are counted, not listed one by one).
// GAINFILES="37:kost_3.7.cu8,87:kost_8.7.cu8,144:...,207:...,280:..." (build 39) -> one recording PER
// TUNER GAIN (tenths of a dB : file), all of the same station, instead of IQFILE. Whenever the app
// sets a gain, the fake dongle goes on with the recording made at the NEAREST of those gains, at
// the same time offset - so GAIN=auto runs the app's real gain search on real signals (a station
// that overloads at 20.7 dB overloads here too), and a "t:again:N" event changes the signal as the
// gain watchdog would. GAINSTEPS=files -> the fake tuner also has ONLY those gain steps (else the
// V4's 29 steps, several of which then share one recording). GAINSTART=2.5 -> start 2.5 s into the
// recordings (to run the same search on another stretch of the same signals).
// apptest's IQSTEP mode sets rtl_stub_after_block: it is then called after every block with
// the recording time played so far, and apptest does all its work there ("virtual time").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include "rtl-sdr.h"
void (*rtl_stub_after_block)(double seconds) = NULL;
volatile int rtl_stub_eof = 0;              // the recording has been played to its end
static FILE *g_file; static volatile int g_cancel; static uint32_t g_freq = 0, g_rate = 1488375; static int g_gain = 300;
static void tr(const char *fmt, ...) {
    static FILE *t; static int tried; if (!tried) { const char *p = getenv("RTLTRACE"); t = p ? fopen(p, "w") : NULL; tried = 1; }
    if (!t) return; va_list a; va_start(a, fmt); vfprintf(t, fmt, a); va_end(a); fputc('\n', t); fflush(t);
}
// ---- GAINFILES (build 39): a recording per gain ----
#define GF_MAX 32
static int g_gfCount = -1;                  // -1 = GAINFILES not read yet, 0 = not set
static int g_gfGain[GF_MAX]; static FILE *g_gfFile[GF_MAX]; static int g_gfNow = -1;
static long g_pos;                          // bytes played so far (the time offset all the files share)
static void gfLoad(void) {
    if (g_gfCount >= 0) return;
    g_gfCount = 0;
    const char *e = getenv("GAINFILES"); if (!e) return;
    if (getenv("GAINSTART")) g_pos = 2 * (long)(atof(getenv("GAINSTART")) * 1488375.0);   // start this many seconds into the recordings
    char *copy = strdup(e);
    for (char *tok = strtok(copy, ","); tok && g_gfCount < GF_MAX; tok = strtok(NULL, ",")) {
        char *colon = strchr(tok, ':'); if (!colon) continue;
        FILE *f = fopen(colon + 1, "rb");
        if (!f) { fprintf(stderr, "STUB: GAINFILES: can't open %s\n", colon + 1); continue; }
        int k = g_gfCount++;                // kept sorted by gain, low to high
        while (k > 0 && g_gfGain[k - 1] > atoi(tok)) { g_gfGain[k] = g_gfGain[k - 1]; g_gfFile[k] = g_gfFile[k - 1]; k--; }
        g_gfGain[k] = atoi(tok); g_gfFile[k] = f;
    }
    free(copy);
}
// The recording for the gain set now: the nearest one. Says so when it changes.
static FILE *gfPick(void) {
    extern int rtl_stub_gain_now(void);
    int best = 0, g = rtl_stub_gain_now();
    for (int k = 1; k < g_gfCount; k++) if (abs(g_gfGain[k] - g) < abs(g_gfGain[best] - g)) best = k;
    if (best != g_gfNow) { g_gfNow = best; if (getenv("GAINFILES_QUIET") == NULL) fprintf(stderr, "STUB: gain %.1f dB -> the %.1f dB recording, from %.2f s\n", g / 10.0, g_gfGain[best] / 10.0, g_pos / 2.0 / 1488375.0); }
    return g_gfFile[best];
}
// Reads the next [len] bytes of "the signal": from IQFILE, or from the GAINFILES recording of the gain set now.
static size_t readIq(void *buf, size_t len) {
    gfLoad();
    if (g_gfCount == 0) {
        if (!g_file) { const char *p = getenv("IQFILE"); g_file = p ? fopen(p, "rb") : NULL; }
        return g_file ? fread(buf, 1, len, g_file) : 0;
    }
    FILE *f = gfPick();
    fseek(f, g_pos, SEEK_SET);
    size_t n = fread(buf, 1, len, f);
    g_pos += (long)n;
    return n;
}
static int haveIq(void) { gfLoad(); if (g_gfCount > 0) return 1; if (!g_file) { const char *p = getenv("IQFILE"); g_file = p ? fopen(p, "rb") : NULL; } return g_file != NULL; }
int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index) { tr("open %u", index); *dev = (rtlsdr_dev_t *)1; return 0; }
// DONGLE=FAIL -> the dongle can't be opened (rtlsdr_open_fd gives -3, as librtlsdr does when libusb can't claim it).
int rtlsdr_open_fd(rtlsdr_dev_t **dev, int fd) { tr("open_fd %d", fd); const char *d = getenv("DONGLE"); if (d && strcmp(d, "FAIL") == 0) { *dev = NULL; return -3; } *dev = (rtlsdr_dev_t *)1; return 0; }
int rtlsdr_close(rtlsdr_dev_t *dev) { (void)dev; tr("close"); return 0; }
// DONGLE=V3 -> pretend to be an RTL-SDR Blog V3 (R820T tuner): below 24 MHz it reports direct sampling, as the real driver does.
static int v3(void) { const char *d = getenv("DONGLE"); return d && strcmp(d, "V3") == 0; }
static int g_gainCalls;   // how often the app touched the tuner gain while in direct sampling (must stay 0)
int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char *m, char *p, char *s) { (void)dev; tr("get_usb_strings"); if (m) strcpy(m, "RTLSDRBlog"); if (p) strcpy(p, v3() ? "Blog V3" : "Blog V4"); if (s) strcpy(s, "00000001"); return 0; }
static int ds(void) { return (v3() && g_freq < 24000000) ? 2 : 0; }
int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev) { (void)dev; tr("get_direct_sampling -> %d", ds()); return ds(); }
int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t f) { (void)dev; tr("set_center_freq %u", f); g_freq = f; return 0; }
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev) { (void)dev; tr("get_center_freq"); return g_freq; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t r) { (void)dev; tr("set_sample_rate %u", r); g_rate = r; return 0; }
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev) { (void)dev; tr("get_sample_rate"); return g_rate; }
enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev) { (void)dev; tr("get_tuner_type"); return v3() ? RTLSDR_TUNER_R820T : RTLSDR_TUNER_R828D; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains) {
    (void)dev; tr("get_tuner_gains %s", gains ? "fill" : "count");
    static const int g[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };
    gfLoad();
    const char *only = getenv("GAINSTEPS");
    if (g_gfCount > 0 && only && strcmp(only, "files") == 0) {       // build 39: only the recorded gains
        if (gains) memcpy(gains, g_gfGain, sizeof(int) * (size_t)g_gfCount);
        return g_gfCount;
    }
    if (gains) memcpy(gains, g, sizeof(g));
    return (int)(sizeof(g) / sizeof(g[0]));
}
static void gainCall(const char *what) { if (ds()) fprintf(stderr, "STUB: %s in direct sampling (call %d) - the app should not do this\n", what, ++g_gainCalls); }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain) { (void)dev; tr("set_tuner_gain %d", gain); gainCall("set_tuner_gain"); g_gain = gain; return 0; }
int rtl_stub_gain_now(void) { return g_gain; }
int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev) { (void)dev; tr("get_tuner_gain"); return g_gain; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual) { (void)dev; tr("set_tuner_gain_mode %d", manual); gainCall("set_tuner_gain_mode"); return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t *dev) { (void)dev; tr("reset_buffer"); return 0; }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm) { (void)dev; (void)ppm; return 0; }
int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read) {
    (void)dev; tr("read_sync %d", len); size_t n = readIq(buf, (size_t)len);
    if (n < (size_t)len) memset((char *)buf + n, 127, (size_t)len - n);
    if (n_read) *n_read = len; return 0;
}
int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len) {
    (void)dev; tr("read_async buf_num %u buf_len %u", buf_num, buf_len); (void)buf_num; if (buf_len == 0) buf_len = 262144;
    unsigned char *buf = (unsigned char *)malloc(buf_len); int f = haveIq(); int pace = getenv("IQPACE") != NULL;
    int loop = getenv("IQLOOP") != NULL;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); double sent = 0; g_cancel = 0;
    while (!g_cancel && f) {
        size_t n = readIq(buf, buf_len);
        if (n < buf_len && loop && g_gfCount == 0 && ftell(g_file) > (long)buf_len) { rewind(g_file); n = fread(buf, 1, buf_len, g_file); }   // IQLOOP: from the top (IQFILE only)
        if (n < buf_len) break;
        cb(buf, buf_len, ctx);
        sent += buf_len / 2.0 / g_rate;
        if (rtl_stub_after_block) rtl_stub_after_block(sent);
        if (pace) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; if (sent > el) usleep((useconds_t)((sent - el) * 1e6)); }
    }
    tr("read_async: %.0f blocks delivered", sent * 2.0 * g_rate / buf_len);
    rtl_stub_eof = 1;
    while (!g_cancel) usleep(20000);       // end of file: wait to be cancelled, like a dongle that went quiet
    tr("read_async returns"); free(buf); return 0;
}
int rtlsdr_cancel_async(rtlsdr_dev_t *dev) { (void)dev; tr("cancel_async"); g_cancel = 1; return 0; }
