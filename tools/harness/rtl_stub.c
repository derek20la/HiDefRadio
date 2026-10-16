// Harness only: a fake RTL-SDR dongle that plays a .cu8 recording ($IQFILE).
// IQPACE=1 -> in real time (as the phone gets it), else as fast as possible.
// IQLOOP=1 -> at the end of the file start again from the top, for ever (a ten-second
// recording becomes ten minutes of listening; the seam is a click, like a retune without one).
// RTLTRACE=calls.txt -> write every driver call the app makes, with its arguments, to that file
// (build 17: the proof that the source split changed nothing the dongle sees - build 16 and
// build 17 must write the same file; sample blocks are counted, not listed one by one).
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
static FILE *file(void) { if (!g_file) { const char *p = getenv("IQFILE"); g_file = p ? fopen(p, "rb") : NULL; } return g_file; }
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
    if (gains) memcpy(gains, g, sizeof(g));
    return (int)(sizeof(g) / sizeof(g[0]));
}
static void gainCall(const char *what) { if (ds()) fprintf(stderr, "STUB: %s in direct sampling (call %d) - the app should not do this\n", what, ++g_gainCalls); }
int rtlsdr_set_tuner_gain(rtlsdr_dev_t *dev, int gain) { (void)dev; tr("set_tuner_gain %d", gain); gainCall("set_tuner_gain"); g_gain = gain; return 0; }
int rtlsdr_get_tuner_gain(rtlsdr_dev_t *dev) { (void)dev; tr("get_tuner_gain"); return g_gain; }
int rtlsdr_set_tuner_gain_mode(rtlsdr_dev_t *dev, int manual) { (void)dev; tr("set_tuner_gain_mode %d", manual); gainCall("set_tuner_gain_mode"); return 0; }
int rtlsdr_reset_buffer(rtlsdr_dev_t *dev) { (void)dev; tr("reset_buffer"); return 0; }
int rtlsdr_set_offset_tuning(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_bias_tee(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_direct_sampling(rtlsdr_dev_t *dev, int on) { (void)dev; (void)on; return 0; }
int rtlsdr_set_freq_correction(rtlsdr_dev_t *dev, int ppm) { (void)dev; (void)ppm; return 0; }
int rtlsdr_read_sync(rtlsdr_dev_t *dev, void *buf, int len, int *n_read) {
    (void)dev; tr("read_sync %d", len); FILE *f = file(); size_t n = f ? fread(buf, 1, (size_t)len, f) : 0;
    if (n < (size_t)len) memset((char *)buf + n, 127, (size_t)len - n);
    if (n_read) *n_read = len; return 0;
}
int rtlsdr_read_async(rtlsdr_dev_t *dev, rtlsdr_read_async_cb_t cb, void *ctx, uint32_t buf_num, uint32_t buf_len) {
    (void)dev; tr("read_async buf_num %u buf_len %u", buf_num, buf_len); (void)buf_num; if (buf_len == 0) buf_len = 262144;
    unsigned char *buf = (unsigned char *)malloc(buf_len); FILE *f = file(); int pace = getenv("IQPACE") != NULL;
    int loop = getenv("IQLOOP") != NULL;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); double sent = 0; g_cancel = 0;
    while (!g_cancel && f) {
        size_t n = fread(buf, 1, buf_len, f);
        if (n < buf_len && loop && ftell(f) > (long)buf_len) { rewind(f); n = fread(buf, 1, buf_len, f); }   // IQLOOP: from the top
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
