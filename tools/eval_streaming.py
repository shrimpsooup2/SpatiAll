"""Measure DirectML correctness/speed and streaming separation quality versus look-ahead.

Streaming scheme (what the C++ engine does): every `hop` seconds run the model on the newest 7.8 s window
ending at "now", keep the output that ends `lookahead` seconds before the window end, crossfade joins.
Quality is reported as SDR (dB) of streamed stems against offline Demucs (apply_model, overlap 0.25).

Usage: python tools/eval_streaming.py song.mp3 --model models/htdemucs.onnx [--seconds 45]
"""

import argparse
import pathlib
import sys
import time

import numpy as np
import onnxruntime as ort
import soundfile as sf
import torch

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import htdemucs_ref as ref  # noqa: E402


def load_audio(path, seconds, offset):
    wav, sr = sf.read(path, dtype="float32", always_2d=True)
    wav = wav.T
    if wav.shape[0] == 1:
        wav = np.repeat(wav, 2, 0)
    wav = wav[:2]
    if sr != ref.SR:
        import torchaudio.functional as AF

        wav = AF.resample(torch.from_numpy(wav), sr, ref.SR).numpy()
    start = int(offset * ref.SR)
    return np.ascontiguousarray(wav[:, start : start + int(seconds * ref.SR)])


def session(model, provider):
    so = ort.SessionOptions()
    if provider == "DmlExecutionProvider":
        so.enable_mem_pattern = False
        so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        so.add_session_config_entry("ep.dml.disable_graph_fusion", "1")  # fused graph is numerically wrong
    sess = ort.InferenceSession(model, so, providers=[provider])
    W = sess.get_inputs()[0].shape[-1]
    return (lambda a, b: sess.run(["freq", "time"], {"mix": a, "spec": b})), W


def sdr(ref_sig, est):
    num = np.sum(ref_sig**2)
    den = np.sum((ref_sig - est) ** 2) + 1e-12
    return 10 * np.log10(num / den + 1e-12)


def offline_separate(run, mix, overlap=0.25):
    """demucs.apply.apply_model with shifts=0: triangle-weighted overlap-add of centred, zero-padded chunks."""
    W = ref.SEGMENT
    N = mix.shape[1]
    stride = int((1 - overlap) * W)
    weight = np.concatenate([np.arange(1, W // 2 + 1), np.arange(W - W // 2, 0, -1)]).astype(np.float64)
    weight /= weight.max()
    out, wsum = None, np.zeros(N)
    for off in range(0, N, stride):
        n = min(W, N - off)
        start = off - (W - n) // 2
        chunk = np.zeros((2, W), dtype=np.float32)
        lo, hi = max(start, 0), min(start + W, N)
        chunk[:, lo - start : hi - start] = mix[:, lo:hi]
        est = ref.separate_window(run, chunk)
        trim = (W - n) // 2
        est = est[..., trim : trim + n]
        if out is None:
            out = np.zeros((est.shape[0], 2, N))
        out[..., off : off + n] += weight[:n] * est
        wsum[off : off + n] += weight[:n]
    return out / wsum


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--model", default="models/htdemucs.onnx", help="streaming model (any window length)")
    ap.add_argument("--ref-model", default="models/htdemucs.onnx", help="7.8 s model for the offline reference")
    ap.add_argument("--cpu-check", action="store_true", help="verify DirectML against CPU first")
    ap.add_argument("--seconds", type=float, default=45)
    ap.add_argument("--offset", type=float, default=30)
    ap.add_argument("--hop", type=float, default=0.5)
    ap.add_argument("--lookaheads", default="0.1,0.25,0.5,1.0,2.0,3.9")
    args = ap.parse_args()

    mix = load_audio(args.audio, args.seconds, args.offset)
    N = mix.shape[1]

    # 1) Offline reference: demucs.apply.apply_model(shifts=0, overlap=0.25) re-implemented on the ONNX graph
    ref_run, _ = session(args.ref_model, "DmlExecutionProvider")
    t0 = time.perf_counter()
    offline = offline_separate(ref_run, mix)
    print(f"offline reference: {time.perf_counter() - t0:.1f}s")
    del ref_run

    # 2) Streaming model: correctness + speed
    dml, W = session(args.model, "DmlExecutionProvider")
    win = mix[:, :W] if N >= W else np.pad(mix, ((0, 0), (0, W - N)))
    b = ref.separate_window(dml, win)  # also warms up (graph compile)
    if args.cpu_check:
        cpu, _ = session(args.model, "CPUExecutionProvider")
        a = ref.separate_window(cpu, win)
        corr = np.corrcoef(a.ravel(), b.ravel())[0, 1]
        print(f"DML vs CPU: max abs diff {np.abs(a - b).max():.2e}, corr {corr:.6f}")
        if not np.isfinite(b).all() or corr < 0.999:
            raise SystemExit("DirectML output is wrong on this GPU")
        del cpu, a
    feeds = (win[None].astype(np.float32), ref.spec_cac(win))
    times = []
    for _ in range(5):
        t0 = time.perf_counter()
        dml(*feeds)
        times.append(time.perf_counter() - t0)
    print(f"window {W / ref.SR:.2f}s: core graph median {np.median(times) * 1000:.0f} ms, min {min(times) * 1000:.0f} ms")

    # 3) Streaming simulation; one model run per hop serves every look-ahead
    hop = int(args.hop * ref.SR)
    xf = int(0.02 * ref.SR)
    las = [float(x) for x in args.lookaheads.split(",")]
    S = offline.shape[0]
    outs = {la: np.zeros((S, 2, N)) for la in las}
    prev_tail = {la: None for la in las}
    padded = np.pad(mix, ((0, 0), (W, 0)))  # silence before the stream starts
    fade = np.linspace(0, 1, xf)
    for t_end in range(hop, N + 1, hop):
        window = padded[:, t_end : t_end + W]  # padded index t_end+W == stream sample t_end
        stems = ref.separate_window(dml, window)
        for la in las:
            L = int(la * ref.SR)
            seg_end = W - L
            seg = stems[..., seg_end - hop : seg_end].copy()  # stream samples [t_end-L-hop, t_end-L)
            # The previous window already estimated this region's first xf samples (past its region end).
            if prev_tail[la] is not None:
                seg[..., :xf] = prev_tail[la] * (1 - fade) + seg[..., :xf] * fade
            prev_tail[la] = stems[..., seg_end : seg_end + xf] if L >= xf else None
            o0 = t_end - L - hop
            if t_end - L <= 0:
                continue  # region lies entirely before the stream start
            o_lo, s_lo = max(o0, 0), max(0, -o0)
            outs[la][..., o_lo : t_end - L] = seg[..., s_lo:]
    warm = int(8 * ref.SR)
    names = ["drums", "bass", "other", "vocals", "guitar", "piano"][:S]
    mix_rms = np.sqrt(np.mean(mix[:, warm:] ** 2)) + 1e-12
    levels = [20 * np.log10(np.sqrt(np.mean(offline[s, :, warm:] ** 2)) / mix_rms + 1e-12) for s in range(S)]
    print(f"\nstem level vs mix (dB; SDR is meaningless for stems far below the mix):")
    print("           " + "  ".join(f"{v:7.1f}" for v in levels))
    print(f"SDR vs offline (dB, higher is better), hop {args.hop}s:")
    print("lookahead  " + "  ".join(f"{n:>7}" for n in names))
    for la in las:
        end = N - int(la * ref.SR)
        vals = [sdr(offline[s, :, warm:end], outs[la][s, :, warm:end]) for s in range(S)]
        print(f"{la:8.2f}s  " + "  ".join(f"{v:7.1f}" for v in vals))


if __name__ == "__main__":
    main()
