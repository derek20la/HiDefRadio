// Harness only: a fake RTL-SDR dongle that plays a .cu8 recording ($IQFILE).
// IQPACE=1 -> in real time (as the phone gets it), else as fast as possible.
// apptest's IQSTEP mode sets rtl_stub_after_block: it is then called after every block with
// the recording time played so far, and apptest does all its work there ("virtual time").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include "rtl-sdr.h"
void (*rtl_stub_after_block)(double seconds) = NULL;
volatile int rtl_stub_eof = 0;              // the recording has been played to its end
static FILE *g_file; static volatile int g_cancel; static uint32_t g_freq = 0, g_rate = 1488375; static int g_gain = 300;
static FILE *file(void) { if (!g_file) { const char *p = getenv("IQFILE"); g_file = p ? fopen(p, "rb") : NULL; } return g_file; }
int rtlsdr_open(rtlsdr_dev_t **dev, uint32_t index) { (void)index; *dev = (rtlsdr_dev_t *)1; return 0; }
int rtlsdr_open_fd(rtlsdr_dev_t **dev, int fd) { (void)fd; *dev = (rtlsdr_dev_t *)1; return 0; }
int rtlsdr_close(rtlsdr_dev_t *dev) { (void)dev; return 0; }
// DONGLE=V3 -> pretend to be an RTL-SDR Blog V3 (R820T tuner): below 24 MHz it reports direct sampling, as the real driver does.
static int v3(void) { const char *d = getenv("DONGLE"); return d && strcmp(d, "V3") == 0; }
static int g_gainCalls;   // how often the app touched the tuner gain while in direct sampling (must stay 0)
int rtlsdr_get_usb_strings(rtlsdr_dev_t *dev, char *m, char *p, char *s) { (void)dev; if (m) strcpy(m, "RTLSDRBlog"); if (p) strcpy(p, v3() ? "Blog V3" : "Blog V4"); if (s) strcpy(s, "00000001"); return 0; }
int rtlsdr_get_direct_sampling(rtlsdr_dev_t *dev) { (void)dev; return (v3() && g_freq < 24000000) ? 2 : 0; }
int rtlsdr_set_center_freq(rtlsdr_dev_t *dev, uint32_t f) { (void)dev; g_freq = f; return 0; }
uint32_t rtlsdr_get_center_freq(rtlsdr_dev_t *dev) { (void)dev; return g_freq; }
int rtlsdr_set_sample_rate(rtlsdr_dev_t *dev, uint32_t r) { (void)dev; g_rate = r; return 0; }
uint32_t rtlsdr_get_sample_rate(rtlsdr_dev_t *dev) { (void)dev; return g_rate; }
enum rtlsdr_tuner rtlsdr_get_tuner_type(rtlsdr_dev_t *dev) { (void)dev; return v3() ? RTLSDR_TUNER_R820T : RTLSDR_TUNER_R828D; }
int rtlsdr_get_tuner_gains(rtlsdr_dev_t *dev, int *gains) {
    (void)dev;
    static const int g[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };
    if (gains) memcpy(gains, g, sizeof(g));
    return (int)(sizeof(g) / sizeof(g[0]));
}
static void gainCall(const char *what) { if (rtlsdr_get_direct_sampling(NULL)) fprintf(stderr, "STUB: %s in direct sampling (call %d) - the app should not do this\n", what, ++g_gainCalls); }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain) { (void)dev; gainCall("set_tuner_gain"); g_gain = gain; return 0; }
int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev) { (void)dev; return g_gain; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual) { (void)dev; (void)manual; gainCall("set_tuner_gain_mode"); return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t *dev) { (void)dev; return 0; }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm) { (void)dev; (void)ppm; return 0; }
int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read) {
    (void)dev; FILE *f = file(); size_t n = f ? fread(buf, 1, (size_t)len, f) : 0;
    if (n < (size_t)len) memset((char *)buf + n, 127, (size_t)len - n);
    if (n_read) *n_read = len; return 0;
}
int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len) {
    (void)dev; (void)buf_num; if (buf_len == 0) buf_len = 262144;
    unsigned char *buf = (unsigned char *)malloc(buf_len); FILE *f = file(); int pace = getenv("IQPACE") != NULL;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); double sent = 0; g_cancel = 0;
    while (!g_cancel && f) {
        size_t n = fread(buf, 1, buf_len, f);
        if (n < buf_len) break;
        cb(buf, buf_len, ctx);
        sent += buf_len / 2.0 / g_rate;
        if (rtl_stub_after_block) rtl_stub_after_block(sent);
        if (pace) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; if (sent > el) usleep((useconds_t)((sent - el) * 1e6)); }
    }
    rtl_stub_eof = 1;
    while (!g_cancel) usleep(20000);       // end of file: wait to be cancelled, like a dongle that went quiet
    free(buf); return 0;
}
int rtlsdr_cancel_async(rtlsdr_dev_t *dev) { (void)dev; g_cancel = 1; return 0; }
