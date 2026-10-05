# Where did the 1 kHz test tone end up? (raw float32 stereo, 44.1 kHz)   python3 tone.py out.f32
import numpy as np, sys
for name in sys.argv[1:]:
    a = np.fromfile(name, np.float32).reshape(-1, 2)[44100*2:]   # skip 2 s
    t = np.arange(len(a))/44100.0
    ref = np.exp(-2j*np.pi*1000*t)
    l = abs((a[:,0]*ref).mean())*2; r = abs((a[:,1]*ref).mean())*2
    print(f"{name}: 1 kHz tone  LEFT out {l:.3f}   RIGHT out {r:.3f}   -> tone is on the {'LEFT' if l>r else 'RIGHT'} ({20*np.log10(max(l,r)/max(min(l,r),1e-9)):.0f} dB separation)")
