# Harness only (build 41): makes "the same signal at another level" out of a .cu8 recording, for
# GAINFILES tests that a real recording can't give (needs numpy).
#
#   python3 scaleiq.py in.cu8 out.cu8 -9.5 12
#       the first 12 s of in.cu8, 9.5 dB quieter. Build 41 used this to replay 11-20 in Long Beach:
#       the 11-23 KDAY files at 40.2 ... 14.4 dB scaled down until their peaks read a "safe"
#       -9 ... -12 dBFS - while the damage the overload did stays in the samples. That is what an
#       overload by stations OUTSIDE our 1.5 MHz looks like to the app: a quiet peak, no HD.
#
#   python3 scaleiq.py --steps 903.cu8 a903 297 4
#       one file per V4 tuner step (a903_0.cu8 ... a903_496.cu8), as if 903.cu8 had been recorded
#       at 29.7 dB: the peak then follows the gain dB for dB, as on a real dongle, and a recording
#       made at one gain can stand in for a whole search. Prints the GAINFILES string to use.
#       (Louder than the recording = clipped at 0 / 255, as the dongle would. Quieter = rounded to
#       8 bits again - roughly what the dongle's own rounding does.)
import sys
import numpy as np

RATE = 1488375
V4_STEPS = [0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338,
            364, 372, 386, 402, 421, 434, 439, 445, 480, 496]


def read(path, seconds):
    """The first [seconds] of a .cu8 file, centred on 0 (the dongle's 127.5)."""
    return np.fromfile(path, dtype=np.uint8, count=int(seconds * RATE) * 2).astype(np.float32) - 127.5


def write(samples, db, path):
    """[samples] made [db] louder (or quieter), back to 8 bits, to [path]."""
    k = 10 ** (db / 20)
    np.clip(np.rint(samples * k + 127.5), 0, 255).astype(np.uint8).tofile(path)


if len(sys.argv) == 6 and sys.argv[1] == "--steps":
    src, prefix, ref, secs = sys.argv[2], sys.argv[3], int(sys.argv[4]), float(sys.argv[5])
    x = read(src, secs)
    for g in V4_STEPS:
        write(x, (g - ref) / 10, f"{prefix}_{g}.cu8")
    print(",".join(f"{g}:{prefix}_{g}.cu8" for g in V4_STEPS))
elif len(sys.argv) == 5:
    write(read(sys.argv[1], float(sys.argv[4])), float(sys.argv[3]), sys.argv[2])
else:
    sys.exit(__doc__ if __doc__ else "see the top of scaleiq.py")
