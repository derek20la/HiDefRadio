// psstitch.hpp - build 37: putting a scrolling PS back together.
//
// Many stations use the 8-character PS ("Programme Service name") as a tiny ticker: the song
// goes through it a word or two at a time. 91X (XETRA 91.1), Z90 (XHITZ 90.3) and Magic 92.5
// (XHRM), recorded in Long Beach on 2026-10-26:
//
//     "  91X   " "  THE   " " MIDDLE " "by Jimmy" "  Eat   " "World on"   (then "91X" again)
//
// and their RadioText only ever holds ONE piece at a time ("THE MIDDLE", then "Jimmy Eat
// World", then an ad for their app). Alt 98.7 (KYSR) slides an 8-letter window instead:
//
//     "Alt 98.7" "Justice " "/ Tame  " "Impala -" "Neverend" "verender"
//
// This class watches the PS frames go by and, once the station has gone round its whole
// message and started again, joins them into one line:
//
//     THE MIDDLE by Jimmy Eat World on 91X
//     Justice / Tame Impala - Neverender Alt 98.7
//
// How (tested on 16 recordings, see claude/Build_37_ps_stitch.md in the project):
//  1. Frames shown for less than MIN_HOLD_S are dropped: while a station switches frames, the
//     decoder can briefly confirm a half-new one ("Mix) b  " on the way to "Mix) by ").
//  2. The message is complete when the last THREE frames repeat three frames seen earlier, in
//     the same order. (Two are not enough: on a weak signal frames go missing, and two in a
//     row gave a wrong loop on one of the Z90 recordings. Three just waits a little longer.)
//  3. Neighbouring frames are joined with a space - unless the end of one is the start of the
//     next (a sliding window: "Neverend" + "verender" = "Neverender").
//  4. Where does the message start? Right after the frame that names the station by its
//     frequency ("91X", "Z90", "92.5", "Alt 98.7") - unless that frame runs on into a word the
//     next one finishes ("99.3 EXI" + "TOS TODO"); without one, where a recent RadioText
//     message starts ("ROSES" in "ROSES by CHURCHES"); else in the order it was heard.
//  5. When two frames in a row are not part of the message any more (a new song), the
//     line is forgotten until the new message has gone round.
//  6. Each place in the message counts the frames seen there: one that got in garbled is
//     replaced by the right one once that has been seen there more often.
//
// What it cannot know: whether two 8-character pieces with no space between them belong to
// one word. "Bell Biv" + "Devoe ON" are two words, "99.3 EXI" + "TOS TODO" is one ("EXITOS").
// A space is the better guess for songs, so it always puts one there.
//
// Everything here runs on the RDS decoder's thread (rds.hpp), a few times a minute at most.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace rds {

class PsStitch {
public:
    static constexpr double MIN_HOLD_S = 0.8;   // a frame shown for less than this is a changeover, not text
    static constexpr int MAX_KEPT = 48;         // frames remembered (91X goes round in 6, Magic in 8)
    static constexpr int RT_KEPT = 4;           // RadioText messages remembered (91X rotates 3)

    // The station's frequency, for step 4 ("which frame is the station's name?"). kHz: 91100.
    void setFreqKhz(int khz) {
        whole_ = khz >= 1000 ? std::to_string(khz / 1000) : std::string();
        tenth_ = (char)('0' + (khz % 1000) / 100);
    }

    // Forget the message (new station, or the PI changed).
    void clear() {
        haveCur_ = false; kept_.clear(); cycle_.clear(); votes_.clear(); pos_ = 0; misses_ = 0;
        rts_.clear(); text_.clear(); rtOther_.clear();
    }

    // The decoder confirmed a new 8-character PS frame at time t (seconds of signal).
    void onFrame(const uint8_t f[8], double t) {
        if (haveCur_ && std::memcmp(cur_.data(), f, 8) == 0) return;   // the same frame again: nothing new
        if (haveCur_ && t - curSince_ >= MIN_HOLD_S) keep(cur_);      // the one it replaces was real
        std::memcpy(cur_.data(), f, 8); curSince_ = t; haveCur_ = true;
    }

    // A new RadioText message (raw RDS characters, end mark already cut off).
    void onRt(const uint8_t *rt, int len) {
        std::string n = norm(rt, len);
        if (n.size() < 3) return;
        for (size_t i = 0; i < rts_.size(); i++) if (rts_[i].norm == n) { rts_.erase(rts_.begin() + i); break; }
        rts_.insert(rts_.begin(), Rt{std::vector<uint8_t>(rt, rt + len), n});
        if ((int)rts_.size() > RT_KEPT) rts_.pop_back();
        if (!cycle_.empty()) makeText();                               // it may give the start (step 4)
    }

    // The whole message, as raw RDS characters ("" until the station has gone round once).
    const std::vector<uint8_t> &text() const { return text_; }
    // The newest RadioText message that is NOT just a piece of text() (91X: its app ad, not
    // "Jimmy Eat World") - shown under it, so the line doesn't flip between the pieces.
    const std::vector<uint8_t> &rtOther() const { return rtOther_; }

private:
    using Frame = std::array<uint8_t, 8>;
    struct Rt { std::vector<uint8_t> raw; std::string norm; };

    void keep(const Frame &f) {
        kept_.push_back(f);
        if ((int)kept_.size() > MAX_KEPT) kept_.pop_front();
        if (!cycle_.empty()) {
            const int n = (int)cycle_.size(), next = (pos_ + 1) % n;
            int at = cycle_[next] == f ? next : -1;                    // where we are in the message
            for (int k = 0; k < n && at < 0; k++) if (cycle_[k] == f) at = k;
            if (at >= 0) { pos_ = at; vote(at, f); misses_ = 0; return; }
            // Not in the message. Either a better copy of the next piece (step 6) ...
            pos_ = next;
            if (vote(next, f) > votesFor(next, cycle_[next])) {
                cycle_[next] = f; misses_ = 0; makeText(); return;
            }
            // ... or the station has moved on (step 5): twice in a row -> start again.
            if (++misses_ < 2) return;
            cycle_.clear(); votes_.clear(); text_.clear(); rtOther_.clear(); misses_ = 0;
        }
        findCycle();
    }

    // Step 6: every place in the message counts which frame it has seen there. A frame that
    // got in wrong (garbled the one time it was heard) is replaced once the right one has been
    // seen there more often - usually on the next time round.
    int vote(int slot, const Frame &f) {
        for (auto &v : votes_[slot]) if (v.first == f) return v.second = std::min(v.second + 1, 8);
        votes_[slot].push_back({f, 1});
        return 1;
    }
    int votesFor(int slot, const Frame &f) const {
        for (const auto &v : votes_[slot]) if (v.first == f) return v.second;
        return 0;
    }

    // Step 2: do the last three frames repeat three earlier ones? Then the frames from that
    // earlier place up to now are the message. The most recent earlier place wins (shortest
    // loop), but a loop needs at least two frames.
    void findCycle() {
        const int n = (int)kept_.size(), i = n - 3;
        if (i < 2) return;
        for (int j = i - 2; j >= 0; j--) {
            if (kept_[j] == kept_[i] && kept_[j + 1] == kept_[i + 1] && kept_[j + 2] == kept_[i + 2]) {
                cycle_.assign(kept_.begin() + j, kept_.begin() + i);
                votes_.assign(cycle_.size(), {});
                for (size_t k = 0; k < cycle_.size(); k++) vote((int)k, cycle_[k]);
                pos_ = 2 % (int)cycle_.size();                          // the newest frame = the loop's third
                misses_ = 0;
                makeText();
                return;
            }
        }
    }

    // Steps 3 and 4.
    void makeText() {
        const int n = (int)cycle_.size();
        int start = -1;
        for (int r = 0; r < n && start < 0; r++) {                     // after the station's own frame
            const Frame &st = cycle_[(r + n - 1) % n];
            if (namesStation(st) && !namesStation(cycle_[r]) && !cutsWord(st, cycle_[r])) start = r;
        }
        for (int r = 0; r < n && start < 0; r++) {                     // where a RadioText message starts
            std::string s = norm(join(r));
            for (const Rt &rt : rts_) if (s.compare(0, rt.norm.size(), rt.norm) == 0) { start = r; break; }
        }
        text_ = join(start < 0 ? 0 : start);
        rtOther_.clear();
        std::string all = norm(text_);
        for (const Rt &rt : rts_)
            if (all.find(rt.norm) == std::string::npos) { rtOther_ = rt.raw; break; }
    }

    // The message from frame r round to frame r-1, spaces tidied.
    std::vector<uint8_t> join(int r) const {
        const int n = (int)cycle_.size();
        std::vector<uint8_t> out;
        for (int k = 0; k < n; k++) {
            const Frame &cur = cycle_[(r + k) % n];
            int from = 0;
            if (k > 0) {
                const Frame &prev = cycle_[(r + k - 1) % n];
                for (int o = 7; o >= 4 && from == 0; o--)              // the window slid: prev ends as cur starts
                    if (std::memcmp(&prev[8 - o], &cur[0], o) == 0 && !blank(&cur[0], o)) from = o;
                if (from == 0) out.push_back(' ');
            }
            // "on 91X" followed by "  91X": the station's name twice in a row - once is enough
            if (from == 0 && k > 0 && namesStation(cur) && endsWith(out, cur)) { out.pop_back(); continue; }
            out.insert(out.end(), cur.begin() + from, cur.end());
        }
        std::vector<uint8_t> tidy;                                     // one space between words, none at the ends
        for (uint8_t c : out) {
            bool sp = c == ' ' || c < 0x20;
            if (sp && (tidy.empty() || tidy.back() == ' ')) continue;
            tidy.push_back(sp ? ' ' : c);
        }
        while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
        return tidy;
    }

    // Step 4: does this frame name the station by its frequency? For 91.1: "91X" yes (91 not
    // part of a longer number), "1991" no. For 98.7: "98.7", "987" and "98" all count.
    bool namesStation(const Frame &f) const {
        if (whole_.empty()) return false;
        const int w = (int)whole_.size();
        for (int p = 0; p + w <= 8; p++) {
            if (std::memcmp(&f[p], whole_.data(), w) != 0) continue;
            if (p > 0 && (isDigit(f[p - 1]) || f[p - 1] == '.')) continue;
            int q = p + w;
            if (q + 1 < 8 && f[q] == '.' && f[q + 1] == (uint8_t)tenth_) q += 2;
            else if (q < 8 && f[q] == (uint8_t)tenth_) q += 1;
            if (q < 8 && isDigit(f[q])) continue;
            return true;
        }
        return false;
    }

    // Éxitos 99.3 scrolls "99.3 EXI" + "TOS TODO": the station's frame runs on into a word
    // ("EXITOS") that the next frame finishes - starting there would cut it ("TOS TODO ...").
    // True when the frame ends in letters that are not the frequency, and the next frame starts
    // straight on with no space. ("Alt 98.7" + "Justice " is fine: it ends with the number.)
    bool cutsWord(const Frame &st, const Frame &next) const {
        if (st[7] == ' ' || next[0] == ' ') return false;
        int w = 7;
        while (w > 0 && st[w - 1] != ' ') w--;                         // the frame's last word
        Frame last; last.fill(' ');
        std::memcpy(last.data(), &st[w], 8 - w);
        return !namesStation(last);
    }

    // Does the text so far end with this frame's words (as a whole word)?
    static bool endsWith(const std::vector<uint8_t> &out, const Frame &f) {
        int a = 0, b = 8;
        while (a < b && f[a] == ' ') a++;
        while (b > a && f[b - 1] == ' ') b--;
        int e = (int)out.size();
        while (e > 0 && out[e - 1] == ' ') e--;
        const int len = b - a;
        if (len == 0 || e < len || std::memcmp(&out[e - len], &f[a], len) != 0) return false;
        return e == len || out[e - len - 1] == ' ';
    }

    static bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }
    static bool blank(const uint8_t *p, int n) { for (int i = 0; i < n; i++) if (p[i] != ' ') return false; return true; }
    // For comparing: capitals, single spaces, no spaces at the ends.
    static std::string norm(const uint8_t *p, int n) {
        std::string s;
        for (int i = 0; i < n; i++) {
            uint8_t c = p[i];
            bool sp = c == ' ' || c < 0x20;
            if (sp) { if (!s.empty() && s.back() != ' ') s += ' '; continue; }
            s += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    static std::string norm(const std::vector<uint8_t> &v) { return norm(v.data(), (int)v.size()); }

    std::string whole_; char tenth_ = '0';
    Frame cur_{}; double curSince_ = 0; bool haveCur_ = false;
    std::deque<Frame> kept_;
    std::vector<Frame> cycle_; int misses_ = 0, pos_ = 0;
    std::vector<std::vector<std::pair<Frame, int>>> votes_;     // per place in the message: frames seen there, how often
    std::vector<Rt> rts_;
    std::vector<uint8_t> text_, rtOther_;
};

} // namespace rds
