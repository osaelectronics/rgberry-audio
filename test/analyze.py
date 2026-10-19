import sys, numpy as np
fs = 1_000_000
d = np.fromfile(sys.argv[1] if len(sys.argv) > 1 else "sim_out.f32", dtype=np.float32).reshape(-1, 2)
d = d[int(0.02*fs):]                      # skip start
n = (len(d)//fs*fs) or len(d)
n = 2**int(np.log2(len(d)))
d = d[:n].astype(np.float64)
w = np.blackman(n)
f = np.fft.rfftfreq(n, 1/fs)
for ch, ftone in ((0, float(sys.argv[2]) if len(sys.argv)>2 else 997), (1, float(sys.argv[3]) if len(sys.argv)>3 else 1999)):
    x = d[:, ch] - d[:, ch].mean()
    X = np.abs(np.fft.rfft(x*w))**2
    band = (f > 20) & (f < 20000)
    k = np.argmin(abs(f-ftone)); sig = X[k-6:k+7].sum()
    tot = X[band].sum(); noise = tot - sig
    # harmonics
    harm = sum(X[np.argmin(abs(f-h*ftone))-6:np.argmin(abs(f-h*ftone))+7].sum() for h in range(2,10) if h*ftone<20000)
    other = 1-ch; ft2 = [997,1999][other] if len(sys.argv)<=3 else float(sys.argv[2+other])
    k2 = np.argmin(abs(f-ft2)); xt = X[k2-6:k2+7].sum()
    amp = np.sqrt(2*sig/ (w**2).sum())
    print(f"ch{ch}: tone {ftone:.0f} Hz  SINAD {10*np.log10(sig/noise):.1f} dB  "
          f"THD {10*np.log10(max(harm,1e-30)/sig):.1f} dB  crosstalk {10*np.log10(max(xt,1e-30)/sig):.1f} dB  "
          f"carrier/HF power >24k: {10*np.log10(X[f>24000].sum()/sig):.1f} dB")
