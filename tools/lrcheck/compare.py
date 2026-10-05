# Compares two decodes of the same broadcast (raw float32 stereo, 44.1 kHz): finds the time
# offset, then says whether left/right and the polarity agree.
#   python3 compare.py a.f32 b.f32 "label"
import numpy as np, sys
from scipy.signal import correlate, butter, sosfilt
def load(n): return np.fromfile(n, np.float32).reshape(-1,2).astype(np.float64)
def compare(na, nb, label, maxlag=44100*8):
    a, b = load(na), load(nb)
    n = max(len(a), len(b)); a = np.pad(a, ((0, n-len(a)), (0, 0))); b = np.pad(b, ((0, n-len(b)), (0, 0)))
    sos = butter(4, [200, 6000], 'bandpass', fs=44100, output='sos')
    a = sosfilt(sos, a, axis=0); b = sosfilt(sos, b, axis=0)
    ma, mb = a.sum(1), b.sum(1)
    seg = slice(0, n)
    c = correlate(ma[seg], mb[seg], 'full', 'fft'); mid = len(ma[seg])-1
    w = c[mid-maxlag:mid+maxlag+1]; k = int(np.argmax(np.abs(w))); lag = k-maxlag
    sign = np.sign(w[k])
    if lag >= 0: a2, b2 = a[lag:], b[:n-lag]
    else:        a2, b2 = a[:n+lag], b[-lag:]
    m = min(len(a2), len(b2)); a2, b2 = a2[44100*2:m-44100*2], b2[44100*2:m-44100*2]
    print(f'   ({len(a2)/44100:.1f} s compared)')
    r = lambda x, y: float(np.corrcoef(x, y)[0,1])
    sa, sb = a2[:,0]-a2[:,1], b2[:,0]-b2[:,1]
    print(f"{label}: lag {lag/44.1:.1f} ms, polarity {'same' if sign>0 else 'INVERTED'}")
    print(f"   mono (L+R) correlation {r(a2.sum(1), b2.sum(1)):+.3f}   side (L-R) correlation {r(sa, sb):+.3f}")
    print(f"   L~L {r(a2[:,0],b2[:,0]):+.3f}  R~R {r(a2[:,1],b2[:,1]):+.3f}  |  L~R {r(a2[:,0],b2[:,1]):+.3f}  R~L {r(a2[:,1],b2[:,0]):+.3f}")
    ss = r(sa, sb) * sign
    print(f"   => left/right are {'the SAME way round' if ss>0 else 'SWAPPED'}  (side/mono energy: {10*np.log10(np.var(sa)/np.var(a2.sum(1))):.1f} dB)")
if __name__ == '__main__': compare(sys.argv[1], sys.argv[2], sys.argv[3])
