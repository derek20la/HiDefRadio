// Harness only: runs the app's whole native engine (native-lib.cpp) on a PC against a .cu8
// recording. Usage:  IQFILE=x.cu8 IQPACE=1 ./apptest <freqHz> <source 0 hd/1 analog/2 auto> <seconds> [events...]
//   WAVOUT=out.wav also saves the audio the app would play.
//   CALLS="KGGI KJNY" = the calls the FCC list has on this frequency (for the RDS call rules).
//   GAIN=auto = start with the app's auto-gain (default: a manual 30.0 dB);  DONGLE=V3 = see rtl_stub.c.
//   events: "t:prog:N" = select program N (0 = HD1) at t seconds;  "t:src:N" = change the audio source
// Prints one line per 0.25 s with the keys of interest from getSignalNative().
#include "native-lib.cpp"
#include <map>
#include <sstream>
struct FakeArray { std::vector<int16_t> s; std::vector<int8_t> b; std::vector<int32_t> i; };
static jstring JNICALL fNewStringUTF(JNIEnv *, const char *t) { return (jstring) new std::string(t); }
static jstring JNICALL fNewString(JNIEnv *, const jchar *c, jsize n) { auto *s = new std::string; for (jsize k = 0; k < n; k++) s->push_back(c[k] < 128 ? (char)c[k] : '?'); return (jstring)s; }
static const char *JNICALL fGetStringUTFChars(JNIEnv *, jstring s, jboolean *) { return ((std::string *)s)->c_str(); }
static void JNICALL fReleaseStringUTFChars(JNIEnv *, jstring, const char *) {}
static jsize JNICALL fGetArrayLength(JNIEnv *, jarray a) { FakeArray *f = (FakeArray *)a; return (jsize)std::max(f->s.size(), std::max(f->b.size(), f->i.size())); }
static void JNICALL fSetShortArrayRegion(JNIEnv *, jshortArray a, jsize st, jsize n, const jshort *buf) { memcpy(((FakeArray *)a)->s.data() + st, buf, (size_t)n * 2); }
static jbyteArray JNICALL fNewByteArray(JNIEnv *, jsize n) { auto *f = new FakeArray; f->b.resize((size_t)n); return (jbyteArray)f; }
static jintArray JNICALL fNewIntArray(JNIEnv *, jsize n) { auto *f = new FakeArray; f->i.resize((size_t)n); return (jintArray)f; }
static void JNICALL fSetByteArrayRegion(JNIEnv *, jbyteArray a, jsize st, jsize n, const jbyte *buf) { memcpy(((FakeArray *)a)->b.data() + st, buf, (size_t)n); }
static void JNICALL fSetIntArrayRegion(JNIEnv *, jintArray a, jsize st, jsize n, const jint *buf) { memcpy(((FakeArray *)a)->i.data() + st, buf, (size_t)n * 4); }
static std::string str(jstring s) { std::string r = *(std::string *)s; delete (std::string *)s; return r; }
#define J(name) Java_io_github_derek20la_hidefradio_RadioEngine_##name
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 1; }
    static JNINativeInterface_ table{};
    table.NewStringUTF = fNewStringUTF; table.NewString = fNewString; table.GetStringUTFChars = fGetStringUTFChars;
    table.ReleaseStringUTFChars = fReleaseStringUTFChars; table.GetArrayLength = fGetArrayLength;
    table.SetShortArrayRegion = fSetShortArrayRegion; table.NewByteArray = fNewByteArray; table.NewIntArray = fNewIntArray;
    table.SetByteArrayRegion = fSetByteArrayRegion; table.SetIntArrayRegion = fSetIntArrayRegion;
    JNIEnv env; env.functions = &table;
    int hz = atoi(argv[1]), src = atoi(argv[2]); double secs = atof(argv[3]);
    struct Ev { double t; std::string what; int n; bool done; };
    std::vector<Ev> evs;
    for (int a = 4; a < argc; a++) { std::stringstream ss(argv[a]); std::string t, w, n; getline(ss, t, ':'); getline(ss, w, ':'); getline(ss, n, ':'); evs.push_back({ atof(t.c_str()), w, atoi(n.c_str()), false }); }
    fprintf(stderr, "%s\n", str(J(openDongleNative)(&env, nullptr, 3)).c_str());
    J(setAudioSourceNative)(&env, nullptr, src);
    J(setBlendUnmatchedNative)(&env, nullptr, JNI_FALSE);
    J(setStereoModeNative)(&env, nullptr, 0);
    // CALLS="KGGI KJNY ...": the call letters licensed on this frequency, as the app's FCC list
    // gives them (Stations.callsOn). Not set = "no list for this frequency".
    std::string calls = getenv("CALLS") ? getenv("CALLS") : "";
    J(setChannelCallsNative)(&env, nullptr, (jstring)&calls, getenv("CALLS") ? JNI_TRUE : JNI_FALSE);
    const char *gainEnv = getenv("GAIN");
    int gain = (gainEnv && strcmp(gainEnv, "auto") == 0) ? -1 : 300;
    fprintf(stderr, "%s\n", str(J(startStreamNative)(&env, nullptr, hz, gain, -1)).c_str());
    std::atomic<bool> run{true};
    std::thread reader([&] {                       // like the AudioPlayer: pulls audio all the time
        FakeArray buf; buf.s.resize(4096);
        // WAVOUT=file.wav: keep what the "AudioPlayer" hears (16-bit stereo, 44,100 Hz)
        const char *wavName = getenv("WAVOUT");
        FILE *wav = wavName ? fopen(wavName, "wb") : nullptr;
        uint32_t bytes = 0;
        auto header = [&] {
            uint32_t rate = 44100, byteRate = rate * 4, fmtLen = 16, riffLen = 36 + bytes; uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
            fseek(wav, 0, SEEK_SET);
            fwrite("RIFF", 1, 4, wav); fwrite(&riffLen, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); fwrite(&fmtLen, 4, 1, wav);
            fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav); fwrite(&byteRate, 4, 1, wav);
            fwrite(&align, 2, 1, wav); fwrite(&bits, 2, 1, wav); fwrite("data", 1, 4, wav); fwrite(&bytes, 4, 1, wav);
        };
        if (wav) header();
        while (run) {
            jint n = J(readAudioNative)(&env, nullptr, (jshortArray)&buf, 100);
            if (wav && n > 0) { fwrite(buf.s.data(), 2, (size_t)n, wav); bytes += (uint32_t)n * 2; }
        }
        if (wav) { header(); fclose(wav); }
    });
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    double nextAlign = 2.0, nextPrint = 0.25;
    while (now() < secs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        double t = now();
        for (Ev &e : evs) if (!e.done && t >= e.t) {
            e.done = true;
            if (e.what == "prog") J(setProgramNative)(&env, nullptr, e.n); else J(setAudioSourceNative)(&env, nullptr, e.n);
            printf("%6.2f  >>> %s %d\n", t, e.what.c_str(), e.n);
        }
        if (t >= nextAlign) { nextAlign += 2.0; J(measureAlignmentNative)(&env, nullptr); }   // Kotlin's alignWatcher
        if (t >= nextPrint) {
            nextPrint += 0.25;
            std::string sig = str(J(getSignalNative)(&env, nullptr));
            std::map<std::string, std::string> kv; std::stringstream ss(sig); std::string line;
            while (getline(ss, line)) { size_t eq = line.find('='); if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1); }
            printf("%6.2f  prog %d  synced %s  align %-9s %7s ms n=%s  blend %-8s toHd %s toAnalog %s  reason '%s'  rds %s '%s' same %s '%s'\n", t, (int)J(getProgramNative)(&env, nullptr),
                   kv["synced"].c_str(), kv["alignState"].c_str(), kv["alignMs"].c_str(), kv["alignCount"].c_str(),
                   kv["blendState"].c_str(), kv["blendToHd"].c_str(), kv["blendToAnalog"].c_str(), kv["blendReason"].c_str(),
                   kv["rdsPi"].c_str(), kv["rdsCall"].c_str(), kv["hdSame"].c_str(), kv["hdSameCall"].c_str());
            if (kv["am"] == "1")       // AM: no analog / alignment / blend - show what matters there instead
                printf("        AM  mode %s  ber %s  kbps %s  gain %s dB (auto %s)  direct sampling %s  peak %s dBFS  '%s'\n", kv["mode"].c_str(), kv["ber"].c_str(),
                       kv["kbps"].c_str(), kv["gainDb"].c_str(), kv["gainAuto"].c_str(), kv["direct"].c_str(), kv["peakDbfs"].c_str(), kv["station"].c_str());
        }
    }
    run = false; reader.join();
    J(stopStreamNative)(&env, nullptr);
    J(closeDongleNative)(&env, nullptr);
    return 0;
}
