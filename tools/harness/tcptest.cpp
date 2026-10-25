// Harness only (1.2): iq::TcpSource alone against rtltcp_replay.py --live, under the thread sanitizer.
#include "iqsource_tcp.hpp"
#include <cstdio>
#include <atomic>
static std::atomic<long> bytes{0};
static void cb(unsigned char *, uint32_t len, void *) { bytes += len; }
int fails = 0;
static void chk(bool ok, const char *w) { printf("[%s] %s\n", ok ? " ok " : "FAIL", w); if (!ok) fails++; }
int main(int argc, char **argv) {
    std::string rep;
    auto s = iq::TcpSource::open("127.0.0.1", atoi(argv[1]), &rep);
    chk(s != nullptr, "connected"); if (!s) return 1;
    chk(s->gainSteps().size() == 29 && s->gainSteps().back() == 496, "29 R820T gain steps known");
    s->setSampleRate(1488375); s->setFrequency(101100000); s->flush(); s->setGainMode(true);
    std::vector<uint8_t> b(32768); int got = 0;
    chk(s->setGain(300) == 0 && s->gain() == 297, "gain 30.0 -> nearest step 29.7");
    chk(s->readBurst(b.data(), (int)b.size(), &got) && got == (int)b.size(), "readBurst got a full burst");
    for (int round = 0; round < 3; round++) {
        bytes = 0;
        std::thread t([&] { s->stream(cb, nullptr); });
        for (int i = 0; i < 20; i++) { s->setGain(i % 2 ? 400 : 200); std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
        s->cancel(); t.join();
        printf("   round %d: %ld bytes streamed\n", round, bytes.load());
        chk(bytes > 0 && bytes % 262144 == 0, "whole blocks streamed while the gain moved");
        s->setFrequency(101100000 + 200000 * (round + 1)); s->flush();     // a retune between streams
    }
    { std::thread t([&] { s->stream(cb, nullptr); }); s->cancel(); t.join(); chk(true, "cancel at once: stream returned"); }
    chk(!s->lost(), "not lost");
    s.reset(); chk(true, "closed cleanly");
    printf("%d failure(s)\n", fails); return fails;
}
