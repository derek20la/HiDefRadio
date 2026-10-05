# Synthetic FM stereo signal built from the textbook formula (ITU-R BS.450 / 47 CFR 73.322):
#   MPX = 0.9*[ (L+R)/2 + (L-R)/2 * sin(2*w*t) ] + 0.1*sin(w*t),  w = 2*pi*19 kHz
#   (the 38 kHz subcarrier crosses zero going UP at every zero crossing of the pilot)
#   positive MPX = carrier frequency goes UP.  Here: a 1 kHz tone on the LEFT only.
import numpy as np, sys
fs = 1488375.0; T = 6.0; n = int(fs*T); t = np.arange(n)/fs
L = np.sin(2*np.pi*1000*t); R = np.zeros(n)
w = 2*np.pi*19000
mpx = 0.9*((L+R)/2 + (L-R)/2*np.sin(2*w*t)) + 0.1*np.sin(w*t)
ph = 2*np.pi*(75000*np.cumsum(mpx)/fs + 300.0*t)          # +300 Hz carrier offset (the DC blocker eats exactly 0 Hz)
iq = 0.5*np.exp(1j*ph)
out = np.empty(2*n, np.uint8); out[0::2] = np.round(iq.real*127.5+127.5); out[1::2] = np.round(iq.imag*127.5+127.5)
out.tofile(sys.argv[1])
