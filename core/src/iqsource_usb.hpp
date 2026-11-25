// iqsource_usb.hpp - the RTL-SDR dongle on the phone's USB port, as an iq::Source (build 17).
//
// This is the ONLY file in the app that talks to librtlsdr (the RTL-SDR Blog fork, vendored
// under external/rtl-sdr-blog, with our rtlsdr_open_fd() addition). Every function below is
// one librtlsdr call - or, where noted, the same two or three calls the app made before
// build 17, in the same order with the same arguments. Moving them here changed nothing the
// dongle sees: the test harness's fake dongle (tools/harness/rtl_stub.c, RTLTRACE=1) logs
// every call it gets, and build 16 and build 17 send it the identical list on every test run.
//
// HOW THE DONGLE GETS OPENED ON ANDROID
// An app may not open a USB device by its path. Android asks the user ("Allow HiDef Radio
// to access the RTL2838UHIDIR?"), then hands RadioService a UsbDeviceConnection; its file
// descriptor (a plain number) comes down here, and rtlsdr_open_fd() builds libusb's handle
// from it. Kotlin keeps the connection open as long as the dongle is in use and closes it
// only AFTER this source has been deleted (RadioService.stopRadio, step 5).
//
// Things worth knowing about this driver (all found the hard way; details in the plan):
//   - The V4 (tuner R828D) tunes AM by switching in its own upconverter below 28.8 MHz -
//     the driver does it, the app just asks for 1260 kHz. It recognises a V4 by its USB
//     names ("RTLSDRBlog" / "Blog V4"), which rtlsdr_open_fd reads like rtlsdr_open does.
//   - The V4 Lite ("Blog V4L", tuner R828S - an R828D with one RF input fewer) has the same
//     upconverter. The R828S answers like an R820T, so the driver reports RTLSDR_TUNER_R820T for
//     it and goes by the USB names instead ("RTLSDRBlog" / "Blog V4L"): no direct sampling, the
//     V4L's own input switching. The app shows it as an R828S (build 20, tunerLabel()).
//     Supported since the fork's commit 61d9927 (2026-03-19), which our copy has.
//   - The V3 and other R820T dongles go to DIRECT SAMPLING below 24 MHz by themselves
//     (input Q); directSampling() reports it after each tune, and the radio then makes no
//     gain calls at all (decision 54).
//   - rtlsdr_set_tuner_gain() picks the nearest step the tuner has, so any number is safe.
//   - rtlsdr_read_async() with 0, 0 = the driver's default buffers: 15 buffers of 262,144
//     bytes (131,072 samples, ~88 ms each). onSamples() is called once per buffer.
#pragma once

#include "iqsource.hpp"
#include "rtl-sdr.h"          // librtlsdr (with our rtlsdr_open_fd patch)
#include <android/log.h>
#include <cstring>
#include <memory>
#include <string>

#define IQ_USB_TAG "HiDefRadio-native"          // the same Logcat tag as native-lib.cpp
#define IQ_USB_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  IQ_USB_TAG, __VA_ARGS__)
#define IQ_USB_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, IQ_USB_TAG, __VA_ARGS__)

namespace iq {

class UsbSource final : public Source {
public:
    // Opens the dongle behind Android's file descriptor `fd`. Returns nullptr if librtlsdr
    // could not (then *report says why). On success *report = describe().
    static std::unique_ptr<UsbSource> open(int fd, std::string *report) {
        IQ_USB_LOGI("rtlsdr_open_fd(fd=%d)...", fd);
        rtlsdr_dev_t *dev = nullptr;
        int r = rtlsdr_open_fd(&dev, fd);
        if (r < 0) {
            IQ_USB_LOGE("rtlsdr_open_fd failed: %d", r);
            *report = "librtlsdr: open FAILED (error " + std::to_string(r) + ")";
            return nullptr;
        }
        std::unique_ptr<UsbSource> src(new UsbSource(dev));

        // Ask the dongle about itself - proves we can really talk to it.
        char manufact[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        rtlsdr_get_usb_strings(dev, manufact, product, serial);
        enum rtlsdr_tuner tuner = rtlsdr_get_tuner_type(dev);
        int gainCount = rtlsdr_get_tuner_gains(dev, nullptr);  // nullptr = just count them

        IQ_USB_LOGI("Opened! manufacturer='%s' product='%s' serial='%s'", manufact, product, serial);
        const std::string tunerText = tunerLabel(tuner, manufact, product);   // build 20: "R828S" on a V4L
        IQ_USB_LOGI("Tuner: %s, %d gain steps", tunerText.c_str(), gainCount);

        // The text the app shows under "Show debug info" - word for word as before build 17
        // (testers send screenshots of it; "Blog V3" / "R820T" in it tells the dongles apart).
        src->describe_ = "librtlsdr: opened OK\n";
        src->describe_ += std::string(manufact) + " " + product + " (serial " + serial + ")\n";
        src->describe_ += "Tuner: " + tunerText + "\n";
        src->describe_ += "Gain steps: " + std::to_string(gainCount);
        *report = src->describe_;
        return src;
    }

    ~UsbSource() override {
        IQ_USB_LOGI("rtlsdr_close()");
        rtlsdr_close(dev_);                        // shuts down librtlsdr + libusb for this dongle
    }

    const char *kind() const override { return "USB"; }
    std::string describe() const override { return describe_; }

    int setSampleRate(uint32_t rate) override { return rtlsdr_set_sample_rate(dev_, rate); }
    uint32_t sampleRate() override { return rtlsdr_get_sample_rate(dev_); }
    int setFrequency(uint32_t hz) override { return rtlsdr_set_center_freq(dev_, hz); }
    uint32_t frequency() override { return rtlsdr_get_center_freq(dev_); }
    void flush() override { rtlsdr_reset_buffer(dev_); }
    // > 0 = direct sampling on input I (1) or Q (2); 0 = the tuner is in use; -1 = error.
    bool directSampling() override { return rtlsdr_get_direct_sampling(dev_) > 0; }

    // Two calls, as librtlsdr wants it: how many, then fill them in.
    std::vector<int> gainSteps() override {
        std::vector<int> gains;
        int count = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (count > 0) {
            gains.resize((size_t)count);
            count = rtlsdr_get_tuner_gains(dev_, gains.data());
            gains.resize(count > 0 ? (size_t)count : 0);
        }
        return gains;
    }
    int setGainMode(bool manual) override { return rtlsdr_set_tuner_gain_mode(dev_, manual ? 1 : 0); }
    int setGain(int tenths) override { return rtlsdr_set_tuner_gain(dev_, tenths); }
    int gain() override { return rtlsdr_get_tuner_gain(dev_); }

    bool readBurst(uint8_t *buf, int len, int *got) override {
        *got = 0;
        return rtlsdr_read_sync(dev_, buf, len, got) == 0 && *got > 0;
    }
    // rtlsdr_read_async() only returns when cancelled - by cancel(), or because the dongle
    // was unplugged (then libusb reports the transfer errors and the driver gives up).
    int stream(SampleFn fn, void *ctx) override {
        return rtlsdr_read_async(dev_, fn, ctx, 0, 0);    // 0, 0 = the driver's default buffers
    }
    void cancel() override {
        IQ_USB_LOGI("rtlsdr_cancel_async()");
        rtlsdr_cancel_async(dev_);
    }

private:
    explicit UsbSource(rtlsdr_dev_t *dev) : dev_(dev) {}
    UsbSource(const UsbSource &) = delete;
    UsbSource &operator=(const UsbSource &) = delete;

    // Turns librtlsdr's tuner number into a readable name.
    static const char *tunerName(enum rtlsdr_tuner t) {
        switch (t) {
            case RTLSDR_TUNER_E4000:  return "Elonics E4000";
            case RTLSDR_TUNER_FC0012: return "Fitipower FC0012";
            case RTLSDR_TUNER_FC0013: return "Fitipower FC0013";
            case RTLSDR_TUNER_FC2580: return "FCI FC2580";
            case RTLSDR_TUNER_R820T:  return "Rafael Micro R820T";
            case RTLSDR_TUNER_R828D:  return "Rafael Micro R828D";
            default:                  return "unknown";
        }
    }

    // Build 20: the tuner's name as the app shows it. The same as tunerName(), except for the
    // RTL-SDR Blog V4 Lite: the driver reports its R828S as an R820T (see the top of this file),
    // and "Tuner: Rafael Micro R820T" would read like a V3. Recognised exactly the way the driver
    // does it (rtlsdr_check_dongle_model: both USB names equal), from the names read above - so
    // no extra driver call (the harness's RTLTRACE call list stays the same).
    static std::string tunerLabel(enum rtlsdr_tuner t, const char *manufact, const char *product) {
        if (t == RTLSDR_TUNER_R820T && std::strcmp(manufact, "RTLSDRBlog") == 0 &&
            std::strcmp(product, "Blog V4L") == 0)
            return "Rafael Micro R828S (RTL-SDR Blog V4 Lite)";
        return tunerName(t);
    }

    rtlsdr_dev_t *dev_;
    std::string describe_;
};

}  // namespace iq
