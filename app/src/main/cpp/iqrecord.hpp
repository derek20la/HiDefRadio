// iqrecord.hpp - 1.3 (build 34): records the raw IQ samples to a file, exactly as they come
// from the source (the USB dongle or an rtl_tcp server).
//
// WHAT IS RECORDED
//   Every byte onSamples() gets, untouched: unsigned 8-bit I, then Q, then I, Q ... ("cu8"),
//   at the radio's sample rate (1,488,375 samples a second = 2,976,750 bytes a second, about
//   180 MB a minute). That is the format nrsc5 reads with "-r", and that most SDR programs
//   open as "raw / cu8 / unsigned 8-bit". The file has no header - the app writes the
//   frequency and the sample rate into its NAME instead.
//
// WHY A THREAD OF ITS OWN
//   The samples arrive on the streaming thread, which must never wait (a late block = a gap
//   in the radio). Writing to storage CAN wait - a busy SD card, a phone saving something
//   else. So the streaming thread only copies each block into a big memory buffer (a "ring",
//   16 MB = about 5.6 s of samples), and a writer thread empties it into the file. If the
//   storage is so slow that the ring fills up anyway, the recording ENDS there (reason
//   TOO_SLOW) instead of leaving a hole in the middle of the file: an IQ file with samples
//   missing would decode wrongly from the hole on. What was written stays a good file.
//
// THE LIFE OF ONE RECORDING
//   start(fd, maxBytes)  - Kotlin hands over an open file (the app chose it with Android's
//                          folder picker); from now on this class owns the fd and closes it.
//   push(buf, len)       - the streaming thread, every block (one atomic read when idle).
//   ... the recording ends by itself when maxBytes are taken (LIMIT), the file can't be
//   written (WRITE_FAILED, e.g. the storage is full) or the writer falls behind (TOO_SLOW);
//   or someone calls finish(): Kotlin's stop (BY_YOU) or stopStreaming() (RADIO_STOPPED:
//   a new station, the radio off). finish() waits until everything taken is in the file.
//   collect()            - Kotlin picks up the result (bytes written, why it ended) and the
//                          recorder is IDLE again, ready for the next one.
//
// THREADS
//   push(): the streaming thread only. start() / finish() / collect() / snapshot(): any
//   thread (Kotlin's, the engine thread) - `ctl` keeps them one at a time. `m` protects the
//   ring and is only held for a copy, never during a write to storage.

#pragma once

#include <algorithm>   // std::min
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <cerrno>
#include <unistd.h>    // write, close

namespace iqrec {

enum State { IDLE = 0, RECORDING = 1, ENDED = 2 };

// Why a recording ended (Kotlin: Recorder.kt turns these into a message).
enum Reason {
    NONE = 0,
    BY_YOU = 1,          // the ⏺ button (or the recording was switched off)
    LIMIT = 2,           // Settings -> IQ recording limit reached
    WRITE_FAILED = 3,    // the file could not be written (storage full, folder gone ...)
    TOO_SLOW = 4,        // the storage could not keep up with 3 MB a second
    RADIO_STOPPED = 5,   // another station was tuned, or the radio was turned off
};

static const size_t RING_BYTES = 16u * 1024u * 1024u;   // ~5.6 s of samples
static const size_t WRITE_CHUNK = 1024u * 1024u;        // at most this much per write()

class Recorder {
public:
    ~Recorder() { finish(RADIO_STOPPED); }

    // Starts a recording into [fd] (now owned by this class). [maxBytes] <= 0 = no limit.
    // 0 = started; 2 = one is going on or has not been collected yet; 3 = no memory for
    // the ring. On 2 and 3 the fd is closed here.
    int start(int fd, long long maxBytes) {
        std::lock_guard<std::mutex> c(ctl_);
        if (state_.load() != IDLE) { ::close(fd); return 2; }
        try {
            ring_.assign(RING_BYTES, 0);
        } catch (const std::bad_alloc &) {
            ring_.clear(); ring_.shrink_to_fit();
            ::close(fd);
            return 3;
        }
        {
            std::lock_guard<std::mutex> l(m_);
            rd_ = wr_ = count_ = 0;
            taken_ = 0;
            maxBytes_ = maxBytes > 0 ? (maxBytes & ~1LL) : 0;   // whole I/Q pairs
            closing_ = false;
        }
        fd_ = fd;
        written_ = 0;
        reason_ = NONE;
        writerDone_ = false;
        state_ = RECORDING;
        accepting_ = true;
        writer_ = std::thread(&Recorder::writerMain, this);
        return 0;
    }

    // The streaming thread: one block of samples.
    void push(const unsigned char *buf, uint32_t len) {
        if (!accepting_.load(std::memory_order_relaxed)) return;   // (the usual case)
        std::lock_guard<std::mutex> l(m_);
        if (!accepting_.load() || closing_) return;
        size_t n = len & ~1u;                                       // whole I/Q pairs
        if (maxBytes_ > 0 && taken_ + (long long)n > maxBytes_) n = (size_t)(maxBytes_ - taken_);
        if (n > ring_.size() - count_) {
            endLocked(TOO_SLOW);                                    // no hole in the file: stop here
            return;
        }
        // Copy in, in one or two pieces (the ring wraps round at its end).
        size_t first = std::min(n, ring_.size() - wr_);
        std::memcpy(&ring_[wr_], buf, first);
        if (n > first) std::memcpy(&ring_[0], buf + first, n - first);
        wr_ = (wr_ + n) % ring_.size();
        count_ += n;
        taken_ += (long long)n;
        if (maxBytes_ > 0 && taken_ >= maxBytes_) endLocked(LIMIT);
        else cv_.notify_one();
    }

    // Ends the recording (if one is going on) and waits until everything taken so far is in
    // the file and the file is closed. The result stays there for collect(). [why] counts
    // only if the recording had not already ended by itself.
    void finish(int why) {
        std::lock_guard<std::mutex> c(ctl_);
        if (state_.load() == IDLE) return;
        {
            std::lock_guard<std::mutex> l(m_);
            if (!closing_) endLocked(why);
        }
        if (writer_.joinable()) writer_.join();
        state_ = ENDED;
    }

    // Kotlin, after it saw ENDED (or after finish(BY_YOU)): the result, and back to IDLE.
    // Returns false if there was nothing to collect.
    bool collect(long long *bytes, int *reason) {
        std::lock_guard<std::mutex> c(ctl_);
        if (state_.load() == IDLE) return false;
        if (state_.load() == RECORDING) {
            // Not ended yet: only if it ended by itself and the writer is done.
            if (!writerDone_.load()) return false;
        }
        if (writer_.joinable()) writer_.join();
        *bytes = written_.load();
        *reason = reason_.load();
        {
            std::lock_guard<std::mutex> l(m_);
            ring_.clear(); ring_.shrink_to_fit();                   // give the 16 MB back
        }
        state_ = IDLE;
        return true;
    }

    // For the screen, any thread: 0 idle, 1 recording, 2 ended (waiting for collect).
    // A recording that ended by itself (LIMIT ...) shows as 2 once its file is closed.
    int state() const {
        int s = state_.load();
        if (s == RECORDING && writerDone_.load()) return ENDED;
        return s;
    }
    long long written() const { return written_.load(); }

private:
    // m_ held: no more samples; the writer empties the ring, closes the file and ends.
    void endLocked(int why) {
        if (reason_.load() == NONE) reason_ = why;
        accepting_ = false;
        closing_ = true;
        cv_.notify_one();
    }

    void writerMain() {
        for (;;) {
            size_t at, n;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return count_ > 0 || closing_; });
                if (count_ == 0) break;                             // closing and empty: done
                at = rd_;
                n = std::min({count_, ring_.size() - rd_, WRITE_CHUNK});
            }
            // Write outside the lock - the streaming thread keeps filling the ring meanwhile.
            // (The bytes [at, at + n) are not touched by push() until count_ goes down.)
            size_t done = 0;
            bool ok = true;
            while (done < n) {
                ssize_t w = ::write(fd_, &ring_[at + done], n - done);
                if (w < 0 && errno == EINTR) continue;
                if (w <= 0) { ok = false; break; }
                done += (size_t)w;
            }
            std::lock_guard<std::mutex> l(m_);
            if (!ok) {
                endLocked(WRITE_FAILED);
                count_ = 0;                                         // the rest can't be written either
                break;
            }
            rd_ = (rd_ + n) % ring_.size();
            count_ -= n;
            written_ += (long long)n;
        }
        ::close(fd_);
        fd_ = -1;
        writerDone_ = true;
    }

    std::mutex ctl_;                     // start / finish / collect one at a time
    std::mutex m_;                       // the ring and the counters below
    std::condition_variable cv_;         // "samples waiting" / "closing"
    std::vector<unsigned char> ring_;
    size_t rd_ = 0, wr_ = 0, count_ = 0; // m_
    long long taken_ = 0, maxBytes_ = 0; // m_
    bool closing_ = false;               // m_
    int fd_ = -1;                        // the writer thread's while it runs
    std::thread writer_;
    std::atomic<int> state_{IDLE};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> writerDone_{false};
    std::atomic<long long> written_{0};
    std::atomic<int> reason_{NONE};
};

}  // namespace iqrec
