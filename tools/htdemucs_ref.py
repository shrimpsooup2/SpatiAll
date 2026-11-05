"""NumPy reference for the pre/post-processing the C++ separator performs around the ONNX core.

The ONNX graph (see export_htdemucs.py) takes the raw mix window and its complex-as-channels spectrogram,
and returns the denormalized spectral-branch output plus the time-branch output. Everything here mirrors
HTDemucs._spec / _magnitude / _mask / _ispec exactly so the C++ port can be checked against it.
Window length N is taken from the input (training length is 343980; shorter exports use multiples of 1024).
"""

import numpy as np

SR = 44100
SEGMENT = 343980  # int(7.8 * 44100), the training length
NFFT = 4096
HOP = 1024
PAD = HOP // 2 * 3  # 1536
BINS = NFFT // 2  # 2048 (Nyquist dropped)

_WINDOW = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(NFFT) / NFFT)).astype(np.float64)  # periodic Hann
_NORM = 1.0 / np.sqrt(NFFT)


def frames_for(n):
    return -(-n // HOP)


def _stft(x):
    """torch.stft(center=True, pad_mode='reflect', normalized=True, onesided) for (C, N) float input."""
    x = np.pad(x, ((0, 0), (NFFT // 2, NFFT // 2)), mode="reflect")
    frames = 1 + (x.shape[-1] - NFFT) // HOP
    idx = np.arange(NFFT)[None, :] + HOP * np.arange(frames)[:, None]
    seg = x[:, idx] * _WINDOW  # (C, frames, NFFT)
    return np.fft.rfft(seg, axis=-1).transpose(0, 2, 1) * _NORM  # (C, bins+1, frames)


def _istft(z, length):
    """torch.istft(center=True, normalized=True, window=hann) for (C, bins+1, frames) complex input."""
    z = z / _NORM
    frames = z.shape[-1]
    seg = np.fft.irfft(z.transpose(0, 2, 1), n=NFFT, axis=-1) * _WINDOW  # (C, frames, NFFT)
    total = NFFT + HOP * (frames - 1)
    out = np.zeros((z.shape[0], total))
    env = np.zeros(total)
    for f in range(frames):
        out[:, f * HOP : f * HOP + NFFT] += seg[:, f]
        env[f * HOP : f * HOP + NFFT] += _WINDOW**2
    start = NFFT // 2
    out = out[:, start : start + length]
    env = env[start : start + length]
    return out / np.where(env > 1e-11, env, 1.0)


def spec_cac(mix):
    """mix (2, N) -> (1, 4, BINS, frames) float32, channels [L.re, L.im, R.re, R.im]."""
    n = mix.shape[-1]
    le = frames_for(n)
    x = np.pad(mix.astype(np.float64), ((0, 0), (PAD, PAD + le * HOP - n)), mode="reflect")
    z = _stft(x)[:, :-1, 2 : 2 + le]  # (2, BINS, le)
    out = np.empty((1, 4, BINS, le), dtype=np.float32)
    out[0, 0::2] = z.real
    out[0, 1::2] = z.imag
    return out


def ispec_cac(freq, n):
    """freq (S, 4, BINS, frames) CaC -> (S, 2, n) time-domain."""
    S, _, _, le = freq.shape
    z = freq[:, 0::2].astype(np.float64) + 1j * freq[:, 1::2].astype(np.float64)  # (S, 2, BINS, le)
    z = np.pad(z, ((0, 0), (0, 0), (0, 1), (2, 2)))
    total = HOP * le + 2 * PAD
    out = np.empty((S, 2, n))
    for s in range(S):
        out[s] = _istft(z[s], total)[:, PAD : PAD + n]
    return out


def separate_window(run, mix):
    """run(mix[1,2,N], spec[1,4,F,T]) -> (freq[1,S,4,F,T], time[1,S,2,N]); returns (S, 2, N)."""
    freq, time = run(mix[None].astype(np.float32), spec_cac(mix))
    return ispec_cac(freq[0], mix.shape[-1]) + time[0]
