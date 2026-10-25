// iqsource_tcp.hpp - an RTL-SDR dongle on ANOTHER computer, over the network (rtl_tcp), as an
// iq::Source (1.2).
//
// WHAT IT IS
// `rtl_tcp` is the small server that comes with librtlsdr: run it on a PC or a Raspberry Pi
// with the dongle plugged in (and the antenna where reception is good), and any program on the
// network can tune that dongle and receive its samples. This file is the app's end of that
// conversation. To the radio it looks exactly like the USB dongle (iqsource_usb.hpp): the same
// cu8 samples in the same 262,144-byte blocks, the same tune and gain calls. Nothing after the
// source - nrsc5, the demodulators, RDS, the aligner, the blend - knows the difference.
// What the listener gains: the antenna can be in the attic, no OTG cable, and the phone can
// charge while the radio plays (it has one USB-C port).
//
// THE PROTOCOL (all of it - it is tiny)
//   - After connecting, the server sends 12 bytes: "RTL0", the tuner type and the number of
//     gain steps (both 32-bit, big-endian; the tuner type is librtlsdr's enum rtlsdr_tuner).
//   - Then it sends samples, raw cu8 (I, Q, I, Q ... one byte each), for as long as the
//     connection is open, at whatever rate it was told.
//   - The client sends commands of 5 bytes: a command number and a 32-bit big-endian value.
//     This source uses four: 0x01 frequency (Hz), 0x02 sample rate, 0x03 gain mode (1 = manual,
//     0 = the tuner's AGC), 0x04 gain (tenths of a dB - the server's librtlsdr picks the
//     nearest step, as on USB).
//
// WHAT THE SERVER CANNOT TELL US (see "WHAT A SOURCE MAY NOT KNOW" in iqsource.hpp)
//   - the exact gain steps: only the tuner type and HOW MANY. For the tuners whose step list is
//     fixed in librtlsdr (R820T / R828D: 29 steps, E4000: 14) the list below is that list; for
//     any other tuner, or a count that doesn't match, gainSteps() is empty and the radio's
//     auto-gain falls back to the tuner's own AGC (as it would with USB).
//   - direct sampling: an RTL-SDR Blog V3 on the server still switches to direct sampling for
//     AM by itself (its librtlsdr does that), but we can't ask - directSampling() says false,
//     and the gain calls then simply do nothing there. Harmless.
//   - the dongle's USB names. describe() says "rtl_tcp at host:port" and the tuner type.
//
// HOW THE BYTES FLOW
// One READER THREAD per connection reads the socket all the time, from connect to close, into
// a buffer. It never stops reading - even between two tunes, or while the radio's streaming
// thread is between blocks - because a client that stops reading makes the server's queue
// grow (rtl_tcp keeps up to 500 blocks = 45 s!) and a retune would then play old samples for
// seconds. While nobody is streaming, the buffer keeps only the newest IDLE_KEEP_S of samples.
// stream() hands the buffer out in BLOCK-byte pieces to the radio's onSamples(), like
// librtlsdr's read_async; readBurst() takes a short piece for the auto-gain.
//
// AFTER A RETUNE (flush) the samples already on their way are of the old frequency: the ones in
// our buffer, in the phone's and the server's network buffers, and in the dongle's USB buffer
// on the server. flush() empties our buffer and throws away the next TUNE_DROP_S as well
// (0.4 s - measured as BYTES, not time, so a test run is the same every time). After a gain
// change readBurst() does the same with BURST_SETTLE_S (0.12 s), so the auto-gain measures
// the new gain, not the old one.
//
// WHEN THE NETWORK IS TOO SLOW: the radio needs 2.98 MB/s (about 24 Mbit/s), steadily. If the
// network can't keep up, stream() waits for the next block (the sound stutters); if the radio
// can't keep up, the buffer stops at STREAM_KEEP_S and the oldest samples are dropped (the
// clock correction sees that as "lost samples", like a USB hiccup). If the connection breaks,
// stream() returns -1 and lost() turns true; the app says so on screen.
//
// Plain C++17 + POSIX sockets, no Android API (the INTERNET permission in the manifest is what
// lets an Android app open a socket). Header only, like iqsource_usb.hpp: no CMake change.
#pragma once

#include "iqsource.hpp"
#include <android/log.h>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define IQ_TCP_TAG "HiDefRadio-native"          // the same Logcat tag as native-lib.cpp
#define IQ_TCP_LOGI(...) __android_log_print(ANDROID_LOG_INFO,  IQ_TCP_TAG, __VA_ARGS__)
#define IQ_TCP_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, IQ_TCP_TAG, __VA_ARGS__)

#ifdef HIDEF_HARNESS
// The desktop harness (tools/harness): its "virtual time" mode is called after every block,
// as with its fake USB dongle (rtl_stub.c), and learns when the recording has ended.
extern "C" { extern void (*rtl_stub_after_block)(double seconds); extern volatile int rtl_stub_eof; }
#endif

namespace iq {

class TcpSource final : public Source {
public:
    static constexpr size_t BLOCK = 262144;            // bytes per onSamples() call (= librtlsdr's default buffer)
    static constexpr double TUNE_DROP_S = 0.4;         // thrown away after a retune (see the top)
    static constexpr double BURST_SETTLE_S = 0.12;     // thrown away before an auto-gain measurement
    static constexpr double IDLE_KEEP_S = 1.0;         // kept while nobody streams
    static constexpr double STREAM_KEEP_S = 3.0;       // at most this far behind while streaming
    static constexpr int CONNECT_TIMEOUT_MS = 4000;
    static constexpr int HEADER_TIMEOUT_MS = 4000;

    // Connects to rtl_tcp at host:port. Returns nullptr if it could not (*report says why, in
    // words for the screen). On success *report = describe().
    static std::unique_ptr<TcpSource> open(const std::string &host, int port, std::string *report) {
        IQ_TCP_LOGI("rtl_tcp: connecting to %s:%d...", host.c_str(), port);
        const std::string where = host + ":" + std::to_string(port);
        if (host.empty() || port <= 0 || port > 65535) {
            *report = "rtl_tcp: no server address set";
            return nullptr;
        }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;                    // IPv4 or IPv6, whatever the name gives
        hints.ai_socktype = SOCK_STREAM;
        const std::string portText = std::to_string(port);
        int g = getaddrinfo(host.c_str(), portText.c_str(), &hints, &res);
        if (g != 0 || res == nullptr) {
            *report = "rtl_tcp: can't find " + host + " (" + gai_strerror(g) + ")";
            IQ_TCP_LOGE("%s", report->c_str());
            return nullptr;
        }
        int fd = -1;
        std::string why = "no address";
        for (addrinfo *a = res; a != nullptr && fd < 0; a = a->ai_next) {
            int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) { why = strerror(errno); continue; }
            if (connectWithTimeout(s, a->ai_addr, a->ai_addrlen, CONNECT_TIMEOUT_MS, &why)) fd = s;
            else ::close(s);
        }
        freeaddrinfo(res);
        if (fd < 0) {
            *report = "rtl_tcp: can't connect to " + where + " (" + why + ")";
            IQ_TCP_LOGE("%s", report->c_str());
            return nullptr;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));    // commands go out at once
        int rcv = 1 << 20;                                              // 1 MB: ~0.35 s of samples in the kernel
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

        uint8_t head[12];
        if (!readExact(fd, head, sizeof(head), HEADER_TIMEOUT_MS) || memcmp(head, "RTL0", 4) != 0) {
            ::close(fd);
            *report = "rtl_tcp: " + where + " answered, but it isn't an rtl_tcp server";
            IQ_TCP_LOGE("%s", report->c_str());
            return nullptr;
        }
        uint32_t tuner = 0, gains = 0;
        memcpy(&tuner, head + 4, 4); memcpy(&gains, head + 8, 4);
        tuner = ntohl(tuner); gains = ntohl(gains);

        std::unique_ptr<TcpSource> src(new TcpSource(fd, where, (int)tuner, (int)gains));
        IQ_TCP_LOGI("rtl_tcp: connected to %s - tuner %s, %u gain steps", where.c_str(), tunerName((int)tuner), gains);
        src->reader_ = std::thread([s = src.get()] { s->readLoop(); });
        *report = src->describe();
        return src;
    }

    ~TcpSource() override {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);                    // wakes the reader if it waits in poll / recv
        cv_.notify_all();
        if (reader_.joinable()) reader_.join();
        ::close(fd_);
        IQ_TCP_LOGI("rtl_tcp: disconnected from %s", where_.c_str());
    }

    const char *kind() const override { return "rtl_tcp"; }
    std::string describe() const override {
        return "rtl_tcp: connected to " + where_ + "\n" +
               "Tuner: " + tunerName(tuner_) + " (as the server reports it)\n" +
               "Gain steps: " + std::to_string(gainCount_) +
               (steps_.empty() && gainCount_ > 0 ? " (values not known - tuner AGC)" : "");
    }
    // The connection broke (server stopped, Wi-Fi gone). The radio reports it.
    bool lost() const override { return lost_.load(); }

    int setSampleRate(uint32_t rate) override {
        rate_ = rate;
        return command(0x02, rate);
    }
    uint32_t sampleRate() override { return rate_; }
    int setFrequency(uint32_t hz) override {
        freq_ = hz;
        return command(0x01, hz);
    }
    uint32_t frequency() override { return freq_; }
    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        buf_.clear(); head_ = 0;
        drop_ = bytesFor(TUNE_DROP_S);
    }
    bool directSampling() override { return false; }      // the server can't tell us (see the top)

    std::vector<int> gainSteps() override { return steps_; }
    int setGainMode(bool manual) override { return command(0x03, manual ? 1u : 0u); }
    int setGain(int tenths) override {
        gain_ = nearestStep(tenths);
        return command(0x04, (uint32_t)tenths);           // the server's driver picks the nearest step too
    }
    int gain() override { return gain_; }

    bool readBurst(uint8_t *out, int len, int *got) override {
        *got = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        buf_.clear(); head_ = 0;
        drop_ = bytesFor(BURST_SETTLE_S);                 // samples from before the last command
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (available() < (size_t)len && !lost_ && !stop_)
            if (cv_.wait_until(lock, until) == std::cv_status::timeout) break;
        const size_t n = std::min(available(), (size_t)len);
        if (n == 0) return false;
        memcpy(out, buf_.data() + head_, n);
        head_ += n;
        *got = (int)n;
        return true;
    }

    int stream(SampleFn fn, void *ctx) override {
        std::vector<uint8_t> block(BLOCK);
#ifdef HIDEF_HARNESS
        double sent = 0;                                   // seconds of samples handed out (harness only)
#endif
        int result = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            streaming_ = true;
        }
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return cancel_ || stop_ || lost_ || available() >= BLOCK; });
                if (cancel_ || stop_) break;
                if (available() < BLOCK) { result = -1; break; }   // lost, and nothing whole left
                memcpy(block.data(), buf_.data() + head_, BLOCK);
                head_ += BLOCK;
            }
            fn(block.data(), (uint32_t)BLOCK, ctx);
#ifdef HIDEF_HARNESS
            const uint32_t rate = rate_.load();
            sent += BLOCK / 2.0 / (rate > 0 ? rate : 1u);
            if (rtl_stub_after_block) rtl_stub_after_block(sent);
#endif
        }
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_ = false;
        cancel_ = false;                                   // (set before this stream began: still honoured above)
        if (result < 0) IQ_TCP_LOGE("rtl_tcp: connection to %s lost", where_.c_str());
#ifdef HIDEF_HARNESS
        if (result < 0) rtl_stub_eof = 1;
#endif
        return result;
    }
    void cancel() override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_ = true;
        cv_.notify_all();
    }

    // librtlsdr's tuner names (rtlsdr_tuner enum order), for describe().
    static const char *tunerName(int t) {
        switch (t) {
            case 1: return "Elonics E4000";
            case 2: return "Fitipower FC0012";
            case 3: return "Fitipower FC0013";
            case 4: return "FCI FC2580";
            case 5: return "Rafael Micro R820T";
            case 6: return "Rafael Micro R828D";
            default: return "unknown";
        }
    }
    // The tuner's gain steps (tenths of a dB) when librtlsdr fixes them for that tuner and the
    // server's count agrees - the lists in librtlsdr's rtlsdr_get_tuner_gains().
    static std::vector<int> knownSteps(int tuner, int count) {
        static const int r82xx[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254,
                                     280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496 };
        static const int e4k[] = { -10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420 };
        std::vector<int> v;
        if ((tuner == 5 || tuner == 6) && count == 29) v.assign(std::begin(r82xx), std::end(r82xx));
        else if (tuner == 1 && count == 14) v.assign(std::begin(e4k), std::end(e4k));
        return v;
    }

private:
    TcpSource(int fd, std::string where, int tuner, int gainCount)
        : fd_(fd), where_(std::move(where)), tuner_(tuner), gainCount_(gainCount),
          steps_(knownSteps(tuner, gainCount)) {}
    TcpSource(const TcpSource &) = delete;
    TcpSource &operator=(const TcpSource &) = delete;

    // Bytes of samples in s seconds at the rate set (even: whole I/Q pairs).
    size_t bytesFor(double s) const {
        const uint32_t rate = rate_.load();
        return (size_t)(s * (rate > 0 ? rate : 1488375u)) * 2;
    }
    size_t available() const { return buf_.size() - head_; }

    int nearestStep(int tenths) const {
        if (steps_.empty()) return tenths;
        int best = steps_[0];
        for (int s : steps_) if (std::abs(s - tenths) < std::abs(best - tenths)) best = s;
        return best;
    }

    int command(uint8_t cmd, uint32_t value) {
        if (lost_) return -1;
        uint8_t msg[5];
        msg[0] = cmd;
        const uint32_t be = htonl(value);
        memcpy(msg + 1, &be, 4);
        std::lock_guard<std::mutex> lock(sendMutex_);      // the engine and the watchdog never interleave bytes
        size_t done = 0;
        while (done < sizeof(msg)) {
            ssize_t n = ::send(fd_, msg + done, sizeof(msg) - done, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { IQ_TCP_LOGE("rtl_tcp: command 0x%02x failed: %s", cmd, strerror(errno)); return -1; }
            done += (size_t)n;
        }
        return 0;
    }

    // The reader thread: socket -> buffer, for the whole life of the connection.
    void readLoop() {
        std::vector<uint8_t> chunk(65536);
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            int r = ::poll(&p, 1, 200);
            if (r < 0 && errno == EINTR) continue;
            if (r == 0) continue;
            ssize_t n = ::recv(fd_, chunk.data(), chunk.size(), 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) break;                                  // closed by the server, or an error
            std::lock_guard<std::mutex> lock(mutex_);
            size_t from = 0, len = (size_t)n;
            if (drop_ > 0) {                                    // after a retune / before a burst
                const size_t d = std::min(drop_, len);
                drop_ -= d; from = d; len -= d;
            }
            if (len == 0) continue;
            if (head_ > 0 && head_ >= buf_.size() / 2) {        // reclaim the part already handed out
                buf_.erase(buf_.begin(), buf_.begin() + (long)head_);
                head_ = 0;
            }
            buf_.insert(buf_.end(), chunk.begin() + (long)from, chunk.begin() + (long)(from + len));
            const size_t keep = bytesFor(streaming_ ? STREAM_KEEP_S : IDLE_KEEP_S);
            if (available() > keep) {                           // too far behind: drop the OLDEST
                size_t extra = (available() - keep) & ~(size_t)1;   // whole I/Q pairs only
                head_ += extra;
                if (streaming_) dropped_ += extra;
            }
            cv_.notify_all();
        }
        if (!stop_) {
            lost_ = true;
            IQ_TCP_LOGE("rtl_tcp: the server closed the connection");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }

    static bool connectWithTimeout(int s, const sockaddr *addr, socklen_t len, int ms, std::string *why) {
        const int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(s, addr, len);
        if (r < 0 && errno != EINPROGRESS) { *why = strerror(errno); return false; }
        if (r < 0) {
            pollfd p{s, POLLOUT, 0};
            r = ::poll(&p, 1, ms);
            if (r == 0) { *why = "no answer"; return false; }
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            if (r < 0 || err != 0) { *why = strerror(r < 0 ? errno : err); return false; }
        }
        fcntl(s, F_SETFL, flags);                          // back to blocking
        return true;
    }
    static bool readExact(int s, uint8_t *out, size_t len, int ms) {
        size_t done = 0;
        while (done < len) {
            pollfd p{s, POLLIN, 0};
            if (::poll(&p, 1, ms) <= 0) return false;
            ssize_t n = ::recv(s, out + done, len - done, 0);
            if (n <= 0) return false;
            done += (size_t)n;
        }
        return true;
    }

    const int fd_;
    const std::string where_;
    const int tuner_, gainCount_;
    const std::vector<int> steps_;
    std::thread reader_;
    std::mutex mutex_, sendMutex_;
    std::condition_variable cv_;
    std::vector<uint8_t> buf_;                 // samples received; buf_[head_ ..] not yet handed out
    size_t head_ = 0, drop_ = 0, dropped_ = 0;
    bool streaming_ = false, cancel_ = false;  // under mutex_
    std::atomic<bool> stop_{false}, lost_{false};
    std::atomic<uint32_t> rate_{0}, freq_{0};
    std::atomic<int> gain_{0};
};

}  // namespace iq
