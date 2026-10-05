// rds.hpp - RDS / RBDS decoder for HiDef Radio (milestone 12).
//
// Input:  the FM "MPX" signal (the discriminator output of fmdemod.hpp, 297,675 samples/s,
//         1.0 = 75 kHz deviation).
// Output: who the station is and what it says - PI code (-> call letters in North America),
//         the 8-character PS name, RadioText, program type, RT+ title / artist.
//
// RDS in one paragraph: a station adds a tiny data signal at 57 kHz (3 x the 19 kHz stereo
// pilot), 1187.5 bits per second (= 57,000 / 48). The bits come in BLOCKS of 26: 16 data bits
// and a 10-bit checkword. Four blocks (A B C D) make a GROUP; block A is always the station's
// PI code, block B says what kind of group it is, C and D carry the payload (2 characters each).
//
// Signal path:
//   MPX x e^(-j 2 pi 57 kHz t)        a free-running 57 kHz oscillator. (Taking 3 x the stereo pilot's
//                                      phase instead was tried and measured 2026-09-30: no gain on
//                                      weak stations, and mono stations - KKLA - send RDS with no pilot.)
//   low-pass 2.4 kHz + decimate by 25  -> 11,907 samples/s complex baseband (~10 samples per bit)
//   matched filter                     the exact shape of one RDS bit (a "biphase" pulse)
//   bit clock                          the phase within the bit where the energy peaks (averaged
//                                      over ~0.3 s; a global search, so it can't lock half a bit off)
//   Costas loop                        locks to the station's exact 57 kHz phase (BPSK: 0 or 180 deg)
//   differential decoding              bit = "did the phase flip?" (so the 180 deg ambiguity is harmless)
//   block sync + checkwords            see Sync below
//   group parsing                      PI, PS, RT, PTY, RT+
//
// What makes a decoder "reliable" (Derek, 2026-09-29: "RDS in SDR# is way more reliable than
// SDR++") is mostly what it does with the checkwords. Here every block is one of
//   CLEAN      (checkword matches),
//   CORRECTED  (a burst of 1-2 wrong bits was repaired - the code allows it, but a repair of
//               pure noise "succeeds" 5 % of the time, so these are not trusted on their own),
//   BAD        (thrown away).
// Text from CLEAN blocks is used at once; text from CORRECTED blocks only when it repeats.
// The PI code needs two clean receptions (or more corrected ones) before anything is shown,
// and groups carrying another PI (a co-channel DX station) are ignored.
//
// Plain C++17, no dependencies, no Android. Not thread-safe: one thread pushes samples and
// reads status().
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace rds {

static const double BIT_RATE = 1187.5;          // bits per second (57 kHz / 48)
static const double SUBCARRIER_HZ = 57000.0;
static const int    DECIM = 25;                 // 297,675 / 25 = 11,907 samples/s after the low-pass

// ---- North American call letters from the PI code (NRSC-4-B, annex D) -----------------------
// K and W stations get their PI from the call letters by a formula, so it can be turned back.
// Returns "" when the PI is not a call-letter code (networks, Canada, Mexico, placeholders).
inline std::string callFromPi(uint16_t pi) {
    // codes that had to avoid a zero nibble are sent in a rearranged form - undo that first
    if ((pi & 0xFFF0) == 0xAFA0 && (pi & 0x000F) < 0x000A) pi = (uint16_t)(pi << 12);      // P 0 0 0
    else if ((pi & 0xFF00) == 0xAF00) pi = (uint16_t)(pi << 8);                            // P1 P2 0 0
    else if ((pi & 0xF000) == 0xA000) pi = (uint16_t)(((pi & 0x0F00) << 4) | (pi & 0x00FF)); // P1 0 P3 P4
    if (pi >= 0x9950 && pi <= 0x9EFF) {                 // the old three-letter calls have a table
        struct Three { uint16_t pi; const char *call; };
        static const Three three[] = {
            {0x9950,"KEX"},{0x9951,"KFH"},{0x9952,"KFI"},{0x9953,"KGA"},{0x9954,"KGO"},{0x9955,"KGU"},
            {0x9956,"KGW"},{0x9957,"KGY"},{0x9958,"KID"},{0x9959,"KIT"},{0x995A,"KJR"},{0x995B,"KLO"},
            {0x995C,"KLZ"},{0x995D,"KMA"},{0x995E,"KMJ"},{0x995F,"KNX"},{0x9960,"KOA"},{0x9964,"KQV"},
            {0x9965,"KSL"},{0x9966,"KUJ"},{0x9967,"KVI"},{0x9968,"KWG"},{0x996B,"KYW"},{0x996D,"WBZ"},
            {0x996E,"WDZ"},{0x996F,"WEW"},{0x9971,"WGL"},{0x9972,"WGN"},{0x9973,"WGR"},{0x9975,"WHA"},
            {0x9976,"WHB"},{0x9977,"WHK"},{0x9978,"WHO"},{0x997A,"WIP"},{0x997B,"WJR"},{0x997C,"WKY"},
            {0x997D,"WLS"},{0x997E,"WLW"},{0x9981,"WOC"},{0x9983,"WOL"},{0x9984,"WOR"},{0x9988,"WWJ"},
            {0x9989,"WWL"},{0x9990,"KDB"},{0x9991,"KGB"},{0x9992,"KOY"},{0x9993,"KPQ"},{0x9994,"KSD"},
            {0x9995,"KUT"},{0x9996,"KXL"},{0x9997,"KXO"},{0x9999,"WBT"},{0x999A,"WGH"},{0x999B,"WGY"},
            {0x999C,"WHP"},{0x999D,"WIL"},{0x999E,"WMC"},{0x999F,"WMT"},{0x99A0,"WOI"},{0x99A1,"WOW"},
            {0x99A2,"WRR"},{0x99A3,"WSB"},{0x99A4,"WSM"},{0x99A5,"KBW"},{0x99A6,"KCY"},{0x99A7,"KDF"},
            {0x99AA,"KHQ"},{0x99AB,"KOB"},{0x99B3,"WIS"},{0x99B4,"WJW"},{0x99B5,"WJZ"},{0x99B9,"WRC"}};
        for (const Three &t : three) if (t.pi == pi) return t.call;
        return "";
    }
    if (pi < 0x1000 || pi > 0x994F) return "";
    char c[5] = { pi <= 0x54A7 ? 'K' : 'W', 0, 0, 0, 0 };
    unsigned v = pi - (pi <= 0x54A7 ? 0x1000u : 0x54A8u);
    c[1] = (char)('A' + v / 676 % 26); c[2] = (char)('A' + v / 26 % 26); c[3] = (char)('A' + v % 26);
    return c;
}

// The catch: many stations (iHeart's, for one) send their PI with the FIRST digit replaced by 1
// (KGB 9991 -> 1991, KMYI 3228 -> 1228; done for the TMC traffic service). Such a PI turns back
// into the wrong letters (1991 -> "KDQF"). So a PI of 1xxx only says "one of these nine":
inline std::vector<std::string> callCandidates(uint16_t pi) {
    std::vector<std::string> out;
    if ((pi >> 12) == 1) {
        for (int d = 1; d <= 9; d++) { std::string c = callFromPi((uint16_t)((d << 12) | (pi & 0x0FFF))); if (!c.empty()) out.push_back(c); }
    } else { std::string c = callFromPi(pi); if (!c.empty()) out.push_back(c); }
    return out;
}
// "Is this PI the station with these call letters?" - hdCall as HD Radio sends it ("KGB-FM", "KRTH").
// For the "is the HD the same station as the analog?" question.
inline bool piMatchesCall(uint16_t pi, const std::string &hdCall) {
    std::string c;
    for (char ch : hdCall) { if (ch == '-' || ch == ' ') break; c += (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch); }
    if (c.empty()) return false;
    for (const std::string &cand : callCandidates(pi)) if (cand == c) return true;
    return false;
}

// ---- RDS characters -> UTF-8 (IEC 62106 basic character set; 0x20-0x7E are ASCII here) -------
inline void appendUtf8(std::string &out, uint8_t c) {
    static const char *const hi[128] = {
        "á","à","é","è","í","ì","ó","ò","ú","ù","Ñ","Ç","Ş","ß","¡","Ĳ",
        "â","ä","ê","ë","î","ï","ô","ö","û","ü","ñ","ç","ş","ǧ","ı","ĳ",
        "ª","α","©","‰","Ǧ","ě","ň","ő","π","€","£","$","←","↑","→","↓",
        "º","¹","²","³","±","İ","ń","ű","µ","¿","÷","°","¼","½","¾","§",
        "Á","À","É","È","Í","Ì","Ó","Ò","Ú","Ù","Ř","Č","Š","Ž","Ð","Ŀ",
        "Â","Ä","Ê","Ë","Î","Ï","Ô","Ö","Û","Ü","ř","č","š","ž","đ","ŀ",
        "Ã","Å","Æ","Œ","ŷ","Ý","Õ","Ø","Þ","Ŋ","Ŕ","Ć","Ś","Ź","Ŧ","ð",
        "ã","å","æ","œ","ŵ","ý","õ","ø","þ","ŋ","ŕ","ć","ś","ź","ŧ"," "};
    if (c >= 0x80) out += hi[c - 0x80];
    else if (c < 0x20 || c == 0x7F) out += ' ';          // control characters
    else out += (char)c;
}
inline std::string toUtf8(const uint8_t *raw, int len, bool trim = true) {
    int a = 0, b = len;
    if (trim) {
        while (b > a && (raw[b - 1] == ' ' || raw[b - 1] < 0x20)) b--;
        while (a < b && (raw[a] == ' ' || raw[a] < 0x20)) a++;
    }
    std::string s;
    for (int i = a; i < b; i++) appendUtf8(s, raw[i]);
    return s;
}

// ---- What the decoder knows right now -------------------------------------------------------
struct Status {
    bool synced = false;        // block sync (the checkwords line up)
    int blerPct = 100;          // blocks that were not clean, last ~1.5 s (0 = perfect)
    bool piOk = false;          // the PI code is confirmed
    uint16_t pi = 0;
    std::string call;           // call letters worked out from the PI ("" if it isn't that kind of PI, or a
                                // 1xxx PI whose candidates never showed up in the station's own text)
    std::string ps;             // the 8-character name / scrolling text ("" until confirmed)
    float psSecs = 0;           // how long the PS has stayed the same (a static name stays; a scrolling one keeps changing)
    std::string rt;             // RadioText (the last complete message)
    std::string title, artist;  // RT+ tags, when the station sends them
    int pty = -1;               // program type 0-31 (-1 = not known); the names differ between RBDS and RDS
    bool tp = false, ta = false; // traffic program / traffic announcement flags
    long groups = 0;            // groups received since the tune
    long blocksClean = 0, blocksFixed = 0, blocksBad = 0;   // totals since the tune (while synced)
};

class Decoder {
public:
    explicit Decoder(double mpxRate = 297675.0) : fsIn(mpxRate) { reset(); }

    // Forget everything (new station).
    void reset() {
        if (lp.empty()) init();
        std::fill(hI.begin(), hI.end(), 0.f); std::fill(hQ.begin(), hQ.end(), 0.f);
        std::fill(mI.begin(), mI.end(), 0.f); std::fill(mQ.begin(), mQ.end(), 0.f);
        pos = 0; decim = 0; mpos = 0; nco = 0;
        std::fill(std::begin(energy), std::end(energy), 0.f);
        clk = 0; clkAdj = 0; prevZi = prevZq = 0; bitsSinceClk = 0;
        theta = 0; freq = 0; amp = 0; prevRe = 0;
        reg = 0; bitNo = 0; synced_ = false; lastHitBit = -1000; lastHitSeq = 0;
        bitsInBlock = 0; blockPos = 0; histPos = 0; cleanInHist = 0; sinceClean = 0;
        std::fill(std::begin(hist), std::end(hist), (uint8_t)0);
        std::fill(std::begin(blkSt), std::end(blkSt), 0);
        nGroups = nClean = nFixed = nBad = 0;
        piOk = false; piCur = 0; piCand = 0; piCnt = 0;
        pty = -1; ptyCand = -1; ptyCnt = 0; tp = ta = false;
        clearText();
        serial_++;
    }

    // One MPX sample. Cost: one table look-up, and a ~140-tap filter on every 25th sample.
    inline void push(float mpx) {
        nco += ncoStep;                                  // 57 kHz, free-running (the Costas loop finds the exact phase)
        uint32_t idx = nco >> 22;
        float c = sinTab[(idx + 256) & 1023], s = sinTab[idx];
        hI[pos] = hI[pos + nLp] = mpx * c;               // x e^(-j phase)
        hQ[pos] = hQ[pos + nLp] = -mpx * s;
        int start = pos + 1;
        pos = (pos + 1 == nLp) ? 0 : pos + 1;
        if (++decim < DECIM) return;
        decim = 0;
        const float *pi_ = &hI[start], *pq = &hQ[start], *h = lp.data();
        float a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
        for (int k = 0; k < nLp; k += 4) {
            a0 += h[k] * pi_[k]; a1 += h[k + 1] * pi_[k + 1]; a2 += h[k + 2] * pi_[k + 2]; a3 += h[k + 3] * pi_[k + 3];
            b0 += h[k] * pq[k]; b1 += h[k + 1] * pq[k + 1]; b2 += h[k + 2] * pq[k + 2]; b3 += h[k + 3] * pq[k + 3];
        }
        baseband((a0 + a1) + (a2 + a3), (b0 + b1) + (b2 + b3));
    }

    // A counter that changes whenever something in status() changed (so the caller can skip the copy).
    uint32_t serial() const { return serial_; }
    bool synced() const { return synced_; }
    int blerPct() const { return synced_ ? 100 - 100 * cleanInHist / HIST : 100; }

    Status status() const {
        Status s;
        s.synced = synced_; s.blerPct = blerPct();
        s.piOk = piOk; s.pi = piCur;
        if (piOk) s.call = (piCur >> 12) == 1 ? callSeen : callFromPi(piCur);
        if (psOk) { s.ps = toUtf8(psPub, 8); s.psSecs = (float)((bitNo - psSince) / BIT_RATE); }
        s.rt = toUtf8(rtPub, rtPubLen);
        s.title = toUtf8(title, titleLen); s.artist = toUtf8(artist, artistLen);
        s.pty = pty; s.tp = tp; s.ta = ta;
        s.groups = nGroups; s.blocksClean = nClean; s.blocksFixed = nFixed; s.blocksBad = nBad;
        return s;
    }

private:
    // ================= signal processing =================
    void init() {
        fs = fsIn / DECIM;
        sinTab.resize(1024);
        for (int i = 0; i < 1024; i++) sinTab[i] = (float)std::sin(2 * M_PI * i / 1024);
        ncoStep = (uint32_t)std::llround(SUBCARRIER_HZ / fsIn * 4294967296.0);
        // Low-pass before the decimation: RDS occupies +-2.4 kHz; everything that would fold onto it
        // from fs - 2.4 kHz up (the top of the stereo L-R signal, a 67 kHz SCA) is 55 dB down.
        {
            double fPass = 2400, fStop = fs - 2400, atten = 55;
            double dw = 2 * M_PI * (fStop - fPass) / fsIn, beta = 0.1102 * (atten - 8.7);
            int n = (int)std::ceil((atten - 8) / (2.285 * dw)) + 1; if (n % 2 == 0) n++;
            double fc = (fPass + fStop) / 2 / fsIn, mid = (n - 1) / 2.0, i0 = bessel(beta), sum = 0;
            std::vector<double> t(n);
            for (int i = 0; i < n; i++) {
                double x = i - mid, sinc = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
                double r = 2 * x / (n - 1);
                t[i] = sinc * bessel(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0; sum += t[i];
            }
            nLp = (n + 3) / 4 * 4;                       // padded with zeros to a multiple of 4
            lp.assign(nLp, 0.f);
            for (int i = 0; i < n; i++) lp[i + (nLp - n)] = (float)(t[n - 1 - i] / sum);   // reversed (symmetric anyway)
            hI.assign(2 * nLp, 0.f); hQ.assign(2 * nLp, 0.f);
        }
        // Matched filter = the shape of one bit. The standard defines it by its spectrum: a pair of
        // impulses (+1 at the start of the bit, -1 half a bit later = "biphase") through a filter
        // cos(pi f td / 4) for |f| < 2/td (td = one bit), split evenly between transmitter and
        // receiver. So: p(t) = the pulse of the half filter sqrt(cos), and h(t) = p(t + td/4) - p(t - td/4).
        {
            double td = 1.0 / BIT_RATE, fmax = 2.0 / td;
            int half = (int)std::lround(1.5 * fs / BIT_RATE);   // +-1.5 bits
            nMf = 2 * half + 1;
            std::vector<double> hmf(nMf);
            const int STEPS = 400;
            auto p = [&](double t) {
                double acc = 0;
                for (int i = 0; i < STEPS; i++) {
                    double f = (i + 0.5) * fmax / STEPS;
                    acc += std::sqrt(std::cos(M_PI * f * td / 4)) * std::cos(2 * M_PI * f * t);
                }
                return acc;
            };
            double e = 0;
            for (int k = 0; k < nMf; k++) {
                double t = (k - half) / fs;
                hmf[k] = p(t + td / 4) - p(t - td / 4); e += hmf[k] * hmf[k];
            }
            mf.resize(nMf);
            for (int k = 0; k < nMf; k++) mf[k] = (float)(hmf[nMf - 1 - k] / std::sqrt(e));
            mI.assign(2 * nMf, 0.f); mQ.assign(2 * nMf, 0.f);
        }
        clkStep = (float)(BIT_RATE / fs);                   // bit phase per sample (~0.0997)
        // Costas loop, updated once per bit: natural frequency 6 Hz, damping 0.707
        double wn = 2 * M_PI * 6.0, zeta = 0.707;
        kp = (float)(2 * zeta * wn / BIT_RATE); ki = (float)(wn * wn / (BIT_RATE * BIT_RATE));
        maxFreq = (float)(2 * M_PI * 20.0 / BIT_RATE);       // +-20 Hz (stations: 57 kHz +-6 Hz)
        // error table: syndrome -> the 1-bit or 2-bit burst that causes it
        std::memset(fixTab, 0, sizeof(fixTab));
        for (int len = 1; len <= 2; len++)
            for (int sh = 0; sh + len <= 26; sh++) {
                uint32_t e = (len == 1 ? 1u : 3u) << sh;
                fixTab[syndrome(e)] = e;
            }
    }
    static double bessel(double x) {
        double sum = 1, term = 1, k = 1;
        while (term > 1e-12 * sum) { double h = x / (2 * k); term *= h * h; sum += term; k += 1; }
        return sum;
    }

    // One complex baseband sample (11,907 per second).
    inline void baseband(float bi, float bq) {
        mI[mpos] = mI[mpos + nMf] = bi; mQ[mpos] = mQ[mpos + nMf] = bq;
        int start = mpos + 1;
        mpos = (mpos + 1 == nMf) ? 0 : mpos + 1;
        const float *xi = &mI[start], *xq = &mQ[start];
        float zi = 0, zq = 0;
        for (int k = 0; k < nMf; k++) { zi += mf[k] * xi[k]; zq += mf[k] * xq[k]; }
        // Bit clock. "clk" runs 0..1 once per bit; a bit is taken where it wraps. The energy of the
        // matched filter's output, sorted by clk into NB bins, peaks at the right moment - the loop
        // slides clk until that peak sits at the wrap (bin 0).
        if (clkAdj != 0.f && clk > 0.45f && clk < 0.56f) {     // steer in the MIDDLE of a bit, far from the wrap,
            clk -= clkAdj; clkAdj = 0.f;                       // so a small correction can never drop or repeat a bit
            while (clk < 0) clk += 1.f;
            while (clk >= 1.f) clk -= 1.f;
        }
        int bin = (int)(clk * NB + 0.5f); if (bin >= NB) bin -= NB;
        energy[bin] += ENERGY_ALPHA * (zi * zi + zq * zq - energy[bin]);
        float next = clk + clkStep;
        if (next >= 1.f) {
            next -= 1.f;
            float frac = (1.f - clk) / clkStep;              // where between the previous sample and this one
            strobe(prevZi + frac * (zi - prevZi), prevZq + frac * (zq - prevZq));
            if (++bitsSinceClk >= 16) { bitsSinceClk = 0; clkAdj = steerClock(); }
        }
        clk = next; prevZi = zi; prevZq = zq;
    }
    // Every 16 bits: where is the energy peak, relative to bin 0?
    // Returns how much to take off clk (applied mid-bit, see baseband()).
    float steerClock() {
        int best = 0;
        for (int b = 1; b < NB; b++) if (energy[b] > energy[best]) best = b;
        float l = energy[(best + NB - 1) % NB], m = energy[best], r = energy[(best + 1) % NB];
        float den = l - 2 * m + r, frac = den < 0 ? 0.5f * (l - r) / den : 0.f;   // parabola through the 3 bins
        float p = best + frac; if (p > NB / 2) p -= NB;                             // in bins, -NB/2..NB/2
        if (std::fabs(p) > 1.5f) {
            // far off (just tuned): jump, and turn the bins along so the average stays valid
            int k = (int)std::lround(p);
            float tmp[NB];
            for (int b = 0; b < NB; b++) tmp[b] = energy[((b + k) % NB + NB) % NB];
            std::memcpy(energy, tmp, sizeof(tmp));
            return (float)k / NB;                            // (costs or repeats a bit - only while acquiring)
        }
        return 0.15f * p / NB;                              // close: nudge
    }
    // One bit instant: z = matched-filter output there.
    inline void strobe(float zi, float zq) {
        float cs = std::cos(theta), sn = std::sin(theta);
        float re = zi * cs + zq * sn, im = zq * cs - zi * sn;    // z x e^(-j theta)
        float mag = std::sqrt(re * re + im * im);
        amp += 0.01f * (mag - amp);
        float e = amp > 1e-9f ? im * (re >= 0 ? 1.f : -1.f) / amp : 0.f;   // BPSK phase error (decision directed)
        if (e > 1.f) e = 1.f; else if (e < -1.f) e = -1.f;
        freq += ki * e;
        if (freq > maxFreq) freq = maxFreq; else if (freq < -maxFreq) freq = -maxFreq;
        theta += freq + kp * e;
        if (theta > (float)M_PI) theta -= 2 * (float)M_PI; else if (theta < -(float)M_PI) theta += 2 * (float)M_PI;
        int bit = (re >= 0) != (prevRe >= 0) ? 1 : 0;          // differential: 1 = the phase flipped
        prevRe = re;
        pushBit(bit);
    }

    // ================= blocks and sync =================
    // A block is 16 data bits + 10 check bits; check = CRC(data) XOR an "offset word" that is
    // different for blocks A, B, C, C' and D - that is how a receiver finds where blocks start.
    static uint16_t crc10(uint16_t data) {                    // remainder of data * x^10 by x^10+x^8+x^7+x^5+x^4+x^3+1
        uint16_t r = 0;
        for (int i = 15; i >= 0; i--) {
            bool top = (r >> 9) & 1, bit = (data >> i) & 1;
            r = (uint16_t)((r << 1) & 0x3FF);
            if (top != bit) r ^= 0x1B9;
        }
        return r;
    }
    static uint16_t syndrome(uint32_t w) { return (uint16_t)(crc10((uint16_t)(w >> 10)) ^ (w & 0x3FF)); }
    // offset words; index 0..4 = A B C D C'
    static uint16_t offsetWord(int i) { static const uint16_t o[5] = {0x0FC, 0x198, 0x168, 0x1B4, 0x350}; return o[i]; }

    void pushBit(int bit) {
        reg = ((reg << 1) | (uint32_t)bit) & 0x3FFFFFF;
        bitNo++;
        uint16_t syn = syndrome(reg);
        // Watch for two clean blocks the right distance apart, in the right order (A B C D A ...).
        // That is how sync is found - and, while synced, how a slipped bit is noticed.
        int seq = -1;
        for (int i = 0; i < 5; i++) if (syn == offsetWord(i)) { seq = i == 4 ? 2 : i; break; }
        bool resync = false;
        if (seq >= 0) {
            long dist = bitNo - lastHitBit;
            if (dist % 26 == 0 && dist / 26 >= 1 && dist / 26 <= 6 && (lastHitSeq + dist / 26) % 4 == seq) {
                bool aligned = synced_ && bitsInBlock == 25 && blockPos == seq;
                if (!synced_ || (!aligned && sinceClean > 10)) resync = true;
            }
            lastHitBit = bitNo; lastHitSeq = seq;
        }
        if (resync) {
            if (!synced_) { std::fill(std::begin(hist), std::end(hist), (uint8_t)0); histPos = 0; cleanInHist = 0; }
            synced_ = true; serial_++;
            std::fill(std::begin(blkSt), std::end(blkSt), 0);
            blockPos = seq; bitsInBlock = 25; sinceClean = 0;   // falls through: this block is evaluated below
        }
        if (!synced_) return;
        if (++bitsInBlock < 26) return;
        bitsInBlock = 0;
        // --- a whole block ---
        int st = 0; uint32_t w = reg;
        int expect = blockPos;                                 // 0..3 = A B C D
        if (expect == 2 && blkSt[1] && (blk[1] & 0x0800)) expect = 4;       // version B groups use C'
        uint16_t s = (uint16_t)(syn ^ offsetWord(expect));
        if (s == 0) st = 2;
        else if (blockPos == 2 && !blkSt[1] && syn == offsetWord(4)) st = 2; // block B unknown: C' is fine too
        else if (fixTab[s]) { w ^= fixTab[s]; st = 1; }
        blk[blockPos] = (uint16_t)(w >> 10); blkSt[blockPos] = st;
        if (st == 2) { nClean++; sinceClean = 0; } else { if (st == 1) nFixed++; else nBad++; sinceClean++; }
        cleanInHist += (st == 2 ? 1 : 0) - (hist[histPos] == 2 ? 1 : 0);
        hist[histPos] = (uint8_t)st; histPos = (histPos + 1) % HIST;
        if ((histPos & 7) == 0) serial_++;                     // the error rate moved
        if (sinceClean >= LOSE_AFTER) { synced_ = false; serial_++; return; }
        if (blockPos == 3) { group(); std::fill(std::begin(blkSt), std::end(blkSt), 0); blockPos = 0; }
        else blockPos++;
    }

    // ================= groups =================
    void clearText() {
        std::memset(psVal, 0, sizeof(psVal)); std::memset(psCnt, 0, sizeof(psCnt)); std::memset(psBit, 0, sizeof(psBit));
        psOk = false; std::memset(psPub, ' ', sizeof(psPub)); psSince = 0;
        rtClear(); rtFlagKnown = false; rtPubLen = 0;
        rtpCode = -1; rtpCodeCand = -1;
        titleLen = artistLen = 0; callSeen.clear();
    }
    void rtClear() {
        std::memset(rtVal, 0, sizeof(rtVal)); std::memset(rtCnt, 0, sizeof(rtCnt));
        std::memset(rtTent, 0, sizeof(rtTent)); std::memset(rtTentOk, 0, sizeof(rtTentOk));
        rtMaxCell = -1; rtWraps = 0; rtLastCell = -1; rtFlagVotes = 0; rtDone = false;
        tagsOk = false; tagCand = 0; tagCandOk = false;
    }

    // A 1xxx PI (see callCandidates): the call letters count only once the station's own text
    // contains one of the candidates ("KSON1037", "... on KGB").
    void lookForCall(const uint8_t *text, int len) {
        if (!piOk || (piCur >> 12) != 1 || !callSeen.empty()) return;
        std::string up;
        for (int i = 0; i < len; i++) up += (char)((text[i] >= 'a' && text[i] <= 'z') ? text[i] - 32 : text[i]);
        for (const std::string &c : callCandidates(piCur))
            if (up.find(c) != std::string::npos) { callSeen = c; serial_++; return; }
    }

    void gotPi(uint16_t p, int weight) {                     // weight: clean block 2, corrected 1
        if (p == piCand) piCnt = std::min(piCnt + weight, 12); else { piCand = p; piCnt = weight; }
        if (!piOk) {
            if (piCnt >= 4) { piOk = true; piCur = p; serial_++; }
        } else if (piCand != piCur && piCnt >= 6) {          // another station took over the frequency
            piCur = piCand; pty = -1; ptyCnt = 0; tp = ta = false; clearText(); serial_++;
        }
    }

    void group() {
        nGroups++;
        if (blkSt[0]) gotPi(blk[0], blkSt[0]);
        if (!piOk) return;                                   // nothing is shown before we know who it is
        if (blkSt[0] && blk[0] != piCur) return;             // somebody else's group (co-channel)
        if (!blkSt[1]) return;                               // without block B we don't know what C and D mean
        uint16_t b = blk[1];
        int type = b >> 12; bool verB = (b & 0x0800) != 0; bool cleanB = blkSt[1] == 2;
        int p = (b >> 5) & 31;
        if (p == ptyCand) ptyCnt = std::min(ptyCnt + blkSt[1], 12); else { ptyCand = p; ptyCnt = blkSt[1]; }
        if (ptyCnt >= 4 && pty != p) { pty = p; serial_++; }
        if (cleanB) { bool t = (b & 0x0400) != 0; if (t != tp) { tp = t; serial_++; } }

        if (type == 0) {                                     // 0A / 0B: the PS name, 2 characters per group
            if (cleanB) { bool t = (b & 0x0010) != 0; if (t != ta) { ta = t; serial_++; } }
            if (blkSt[3]) psSegment(b & 3, blk[3], (cleanB && blkSt[3] == 2) ? 2 : 1);
        } else if (type == 2) {                              // 2A / 2B: RadioText
            radioText(b, verB, cleanB);
        } else if (type == 3 && !verB) {                     // 3A: "application X uses group type Y"
            if (blkSt[3] && blk[3] == 0x4BD7) {              // RT+ (artist / title tags)
                int code = b & 0x1F;
                if ((cleanB && blkSt[3] == 2) || code == rtpCodeCand) rtpCode = code;
                rtpCodeCand = code;
            }
        } else if (rtpCode >= 0 && ((b >> 11) & 0x1F) == rtpCode) {
            if (blkSt[2] && blkSt[3]) rtPlus(b, cleanB && blkSt[2] == 2 && blkSt[3] == 2);
        }
    }

    // PS: four segments of 2 characters. A segment counts once it has been received twice the
    // same (a clean block counts double); the name is shown when all four are confirmed and
    // fresh. That also copes with "scrolling PS" (text that changes every few seconds): a new
    // frame replaces the old one only when all of it has been confirmed, never a mix of two.
    void psSegment(int a, uint16_t v, int weight) {
        if (psCnt[a] > 0 && psVal[a] == v) psCnt[a] = std::min(psCnt[a] + weight, 8); else { psVal[a] = v; psCnt[a] = weight; }
        psBit[a] = bitNo;
        for (int i = 0; i < 4; i++)
            if (psCnt[i] < 3 || bitNo - psBit[i] > PS_FRESH_BITS) return;
        uint8_t now[8];
        for (int i = 0; i < 4; i++) { now[2 * i] = (uint8_t)(psVal[i] >> 8); now[2 * i + 1] = (uint8_t)psVal[i]; }
        if (psOk && std::memcmp(now, psPub, 8) == 0) return;
        std::memcpy(psPub, now, 8); psOk = true; psSince = bitNo; serial_++;
        lookForCall(psPub, 8);
    }

    // RadioText: up to 64 characters (2A: 4 per group) or 32 (2B: 2 per group), stored here as
    // "cells" of 2 characters. The A/B flag flips when the station starts a new message.
    void radioText(uint16_t b, bool verB, bool cleanB) {
        bool flag = (b & 0x0010) != 0; int addr = b & 15;
        if (!rtFlagKnown) { rtFlag = flag; rtFlagKnown = true; }
        else if (flag != rtFlag) {
            if (cleanB || ++rtFlagVotes >= 2) { rtClear(); rtFlag = flag; }
            else return;                                     // one doubtful block doesn't wipe the text
        } else rtFlagVotes = 0;
        int first = verB ? addr : 2 * addr;
        if (first < rtLastCell) rtWraps++;
        rtLastCell = first;
        if (!verB && blkSt[2]) rtCell(2 * addr, blk[2], (cleanB && blkSt[2] == 2) ? 2 : 1);
        if (blkSt[3]) rtCell(verB ? addr : 2 * addr + 1, blk[3], (cleanB && blkSt[3] == 2) ? 2 : 1);
        // complete? = every cell confirmed up to the end mark (0x0D), or all of them, or - for
        // stations that send a short text without an end mark - everything up to the highest
        // address seen, once the addresses have gone round twice.
        int nCells = verB ? 16 : 32, len = -1, c = 0;
        for (; c < nCells; c++) {
            if (rtCnt[c] < 2) break;
            if ((rtVal[c] >> 8) == 0x0D) { len = 2 * c; break; }
            if ((rtVal[c] & 0xFF) == 0x0D) { len = 2 * c + 1; break; }
        }
        if (len < 0 && c == nCells) len = 2 * nCells;
        if (len < 0 && c > rtMaxCell && rtMaxCell >= 0 && rtWraps >= 2) len = 2 * (rtMaxCell + 1);
        if (len < 0) return;
        uint8_t now[64];
        for (int i = 0; i < len; i++) now[i] = (uint8_t)((i & 1) ? rtVal[i / 2] : rtVal[i / 2] >> 8);
        if (!rtDone || len != rtPubLen || std::memcmp(now, rtPub, len) != 0) {
            bool same = len == rtPubLen && std::memcmp(now, rtPub, len) == 0;
            std::memcpy(rtPub, now, len); rtPubLen = len; rtDone = true;
            if (!same) { titleLen = artistLen = 0; serial_++; lookForCall(rtPub, rtPubLen); }
            applyTags();
        }
    }
    void rtCell(int cell, uint16_t v, int weight) {
        if (cell > rtMaxCell) rtMaxCell = cell;
        if (rtCnt[cell] >= 2) {                              // already confirmed
            if (rtVal[cell] == v) return;
            if (weight < 2) {                                // a doubtful block disagrees: wait for it to repeat
                if (!(rtTentOk[cell] && rtTent[cell] == v)) { rtTent[cell] = v; rtTentOk[cell] = true; return; }
            }
            // really different: the station changed the text without flipping the A/B flag
            bool f = rtFlag; rtClear(); rtFlag = f;
            if (cell > rtMaxCell) rtMaxCell = cell;
            rtVal[cell] = v; rtCnt[cell] = 2;
        } else if (rtCnt[cell] > 0 && rtVal[cell] == v) rtCnt[cell] += weight;
        else { rtVal[cell] = v; rtCnt[cell] = weight; }
    }

    // RT+: "characters 12..25 of the RadioText are the title, 0..8 the artist". Two tags per group.
    void rtPlus(uint16_t b, bool clean) {
        uint64_t bits = ((uint64_t)(b & 0x1F) << 32) | ((uint64_t)blk[2] << 16) | blk[3];
        if (!clean && !(tagCandOk && tagCand == bits)) { tagCand = bits; tagCandOk = true; return; }
        tagCand = bits; tagCandOk = true;
        tagRunning = (bits >> 35) & 1;
        tagType[0] = (int)((bits >> 29) & 63); tagStart[0] = (int)((bits >> 23) & 63); tagLen[0] = (int)((bits >> 17) & 63) + 1;
        tagType[1] = (int)((bits >> 11) & 63); tagStart[1] = (int)((bits >> 5) & 63);  tagLen[1] = (int)(bits & 31) + 1;
        tagsOk = true;
        applyTags();
    }
    void applyTags() {
        if (!tagsOk || !rtDone) return;                      // tags only make sense on the text they came with
        uint8_t t[64], a[64]; int tl = 0, al = 0;
        if (tagRunning)
            for (int i = 0; i < 2; i++) {
                if (tagStart[i] + tagLen[i] > rtPubLen) continue;
                if (tagType[i] == 1) { tl = tagLen[i]; std::memcpy(t, rtPub + tagStart[i], tl); }        // ITEM.TITLE
                else if (tagType[i] == 4) { al = tagLen[i]; std::memcpy(a, rtPub + tagStart[i], al); }   // ITEM.ARTIST
            }
        if (tl != titleLen || al != artistLen || std::memcmp(t, title, tl) != 0 || std::memcmp(a, artist, al) != 0) {
            std::memcpy(title, t, tl); titleLen = tl; std::memcpy(artist, a, al); artistLen = al; serial_++;
        }
    }

    // ----- state -----
    static const int NB = 12;                                // clock bins per bit
    static constexpr float ENERGY_ALPHA = 0.0015f;            // each bin is hit ~0.84 x per bit -> ~0.25 s average
    static const int HIST = 64;                              // blocks in the error-rate window (~1.4 s)
    static const int LOSE_AFTER = 70;                        // blocks without a single clean one -> sync lost (~1.5 s)
    static const long PS_FRESH_BITS = 5938;                  // 5 s

    double fsIn, fs = 0;
    std::vector<float> sinTab, lp, hI, hQ, mf, mI, mQ;
    int nLp = 0, nMf = 0, pos = 0, decim = 0, mpos = 0;
    uint32_t nco = 0, ncoStep = 0;
    float energy[NB] = {}, clk = 0, clkAdj = 0, clkStep = 0, prevZi = 0, prevZq = 0;
    int bitsSinceClk = 0;
    float theta = 0, freq = 0, kp = 0, ki = 0, maxFreq = 0, amp = 0, prevRe = 0;

    uint32_t reg = 0, fixTab[1024];
    long bitNo = 0, lastHitBit = -1000;
    int lastHitSeq = 0, bitsInBlock = 0, blockPos = 0, histPos = 0, cleanInHist = 0, sinceClean = 0;
    bool synced_ = false;
    uint8_t hist[HIST] = {};
    uint16_t blk[4] = {}; int blkSt[4] = {};
    long nGroups = 0, nClean = 0, nFixed = 0, nBad = 0;

    bool piOk = false; uint16_t piCur = 0, piCand = 0; int piCnt = 0;
    int pty = -1, ptyCand = -1, ptyCnt = 0; bool tp = false, ta = false;
    uint16_t psVal[4] = {}; int psCnt[4] = {}; long psBit[4] = {}, psSince = 0;
    bool psOk = false; uint8_t psPub[8] = {};
    uint16_t rtVal[32] = {}, rtTent[32] = {}; int rtCnt[32] = {}; bool rtTentOk[32] = {};
    bool rtFlag = false, rtFlagKnown = false, rtDone = false;
    int rtFlagVotes = 0, rtMaxCell = -1, rtWraps = 0, rtLastCell = -1, rtPubLen = 0;
    uint8_t rtPub[64] = {};
    int rtpCode = -1, rtpCodeCand = -1;
    bool tagsOk = false, tagCandOk = false, tagRunning = false; uint64_t tagCand = 0;
    int tagType[2] = {}, tagStart[2] = {}, tagLen[2] = {};
    uint8_t title[64] = {}, artist[64] = {}; int titleLen = 0, artistLen = 0;
    std::string callSeen;
    uint32_t serial_ = 0;
};

} // namespace rds
