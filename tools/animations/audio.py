"""Audio helpers for the voice mod (numpy only): band-limited resampling, ITU-R BS.1770 loudness and
true peak."""
import numpy as np


def resample(x, src, dst, half_taps=48, beta=8.6):
    """Kaiser-windowed sinc interpolation from src to dst Hz. The cutoff sits a little below the lower
    Nyquist, so downsampling does not alias; the signal is not treated as periodic (no wrap-around at
    the ends, which an FFT resample has)."""
    x = np.asarray(x, dtype=np.float64)
    if src == dst or len(x) == 0:
        return x.copy()
    ratio = dst / src
    cutoff = 0.94 * min(1.0, ratio)  # of the source's Nyquist
    n_out = int(round(len(x) * ratio))
    t = np.arange(n_out) / ratio  # output instants in source samples
    base = np.floor(t).astype(np.int64)
    out = np.zeros(n_out)
    padded = np.concatenate([np.zeros(half_taps), x, np.zeros(half_taps + 1)])
    norm = np.i0(beta)
    for k in range(-half_taps + 1, half_taps + 1):
        idx = base + k
        d = t - idx
        w = np.i0(beta * np.sqrt(np.clip(1.0 - (d / half_taps) ** 2, 0.0, 1.0))) / norm
        out += padded[idx + half_taps] * cutoff * np.sinc(cutoff * d) * w
    return out


def _biquad(x, b, a):
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2 = b
    _, a1, a2 = a
    for i, v in enumerate(x):
        r = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, r
        y[i] = r
    return y


def _k_weighting(rate):
    """BS.1770-4 pre-filter (high shelf) and RLB high pass, for any rate (bilinear design)."""
    f0, g, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = np.tan(np.pi * f0 / rate)
    vh = 10 ** (g / 20)
    vb = vh ** 0.4996667741545416
    a0 = 1 + k / q + k * k
    shelf = ((vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0), \
        (1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0)
    f0, q = 38.13547087602444, 0.5003270373238773
    k = np.tan(np.pi * f0 / rate)
    a0 = 1 + k / q + k * k
    hp = (1.0, -2.0, 1.0), (1.0, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0)
    return shelf, hp


def loudness(x, rate):
    """Integrated loudness in LUFS (BS.1770: K-weighting, 400 ms blocks with 75 % overlap, absolute gate
    -70 LUFS, relative gate -10 LU). Clips shorter than a block are measured as one block."""
    x = np.asarray(x, dtype=np.float64)
    (b1, a1), (b2, a2) = _k_weighting(rate)
    y = _biquad(_biquad(x, b1, a1), b2, a2)
    block, step = int(0.4 * rate), int(0.1 * rate)
    if len(y) <= block:
        powers = np.array([np.mean(y ** 2)])
    else:
        powers = np.array([np.mean(y[s:s + block] ** 2) for s in range(0, len(y) - block + 1, step)])
    lk = -0.691 + 10 * np.log10(np.maximum(powers, 1e-12))
    gated = powers[lk > -70]
    if not len(gated):
        return -70.0
    rel = -0.691 + 10 * np.log10(np.mean(gated)) - 10
    gated = powers[(lk > -70) & (lk > rel)]
    return float(-0.691 + 10 * np.log10(np.mean(gated)))


def true_peak(x, rate):
    """Peak of the signal oversampled 4x (dBTP once in dB): the level the reconstructed wave reaches."""
    return float(np.abs(resample(x, rate, rate * 4, half_taps=24)).max()) if len(x) else 0.0


def bandwidth(x, rate, floor_db=-60.0):
    """Highest frequency whose smoothed spectrum stays within floor_db of the spectrum's peak."""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < 64:
        return 0.0
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    db = 20 * np.log10(np.maximum(np.convolve(spec, np.ones(32) / 32, mode='same'), 1e-12))
    above = np.nonzero(db > db.max() + floor_db)[0]
    return float(above[-1] * rate / 2 / (len(spec) - 1)) if len(above) else 0.0
