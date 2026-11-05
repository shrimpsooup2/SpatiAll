"""Export Demucs v4 (htdemucs / htdemucs_6s) to ONNX with STFT/iSTFT moved out of the graph.

Graph I/O (fixed shapes, float32):
  inputs : mix  [1, 2, 343980]          raw stereo window at 44.1 kHz
           spec [1, 4, 2048, 336]       complex-as-channels STFT of mix (see htdemucs_ref.spec_cac)
  outputs: freq [1, S, 4, 2048, 336]    spectral-branch estimate (CaC, denormalized)
           time [1, S, 2, 343980]       time-branch estimate (denormalized)
  stems = ispec_cac(freq) + time

Usage: python tools/export_htdemucs.py --model htdemucs --out models/htdemucs.onnx [--check]
"""

import argparse
import json
import pathlib
import sys
import time

import numpy as np
import torch
from torch import nn

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import htdemucs_ref as ref  # noqa: E402


class HTDemucsCore(nn.Module):
    """HTDemucs.forward from normalization to denormalization, without _spec/_mask/_ispec."""

    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, mix, mag):
        m = self.m
        x = mag
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)

        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)

        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)

        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_upsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_downsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_downsampler_t(xt)

        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)

        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T)
        x = x * std[:, None] + mean[:, None]
        xt = xt.view(B, S, -1, mix.shape[-1])
        xt = xt * stdt[:, None] + meant[:, None]
        return x, xt


def load(name):
    from demucs.pretrained import get_model

    bag = get_model(name)
    models = getattr(bag, "models", [bag])
    if len(models) != 1:
        raise SystemExit(f"{name} is a bag of {len(models)} models; export a single-model checkpoint")
    m = models[0].eval()
    m.segment = float(m.segment)
    return m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="htdemucs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--segment", type=int, default=ref.SEGMENT,
                    help="window length in samples; shorter than training length must be a multiple of 1024")
    ap.add_argument("--check", action="store_true", help="compare ONNX (CPU) against the PyTorch model")
    args = ap.parse_args()
    if args.segment != ref.SEGMENT and (args.segment % 1024 or args.segment > ref.SEGMENT):
        raise SystemExit("--segment must be a multiple of 1024 below 343980, or exactly 343980")

    torch.backends.mha.set_fastpath_enabled(False)  # aten::_native_multi_head_attention has no ONNX symbolic
    m = load(args.model)
    m.use_train_segment = args.segment == ref.SEGMENT  # else run natively at the shorter length
    core = HTDemucsCore(m).eval()

    rng = np.random.default_rng(0)
    mix = (rng.standard_normal((2, args.segment)) * 0.1).astype(np.float32)
    spec = ref.spec_cac(mix)
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    with torch.no_grad():
        torch.onnx.export(
            core,
            (torch.from_numpy(mix[None]), torch.from_numpy(spec)),
            str(out),
            input_names=["mix", "spec"],
            output_names=["freq", "time"],
            opset_version=args.opset,
            dynamo=False,
        )
    meta = {"sources": list(m.sources), "samplerate": m.samplerate, "segment": args.segment}
    out.with_suffix(".json").write_text(json.dumps(meta, indent=2))
    print(f"exported {out} ({out.stat().st_size / 1e6:.1f} MB), sources={m.sources}")

    if args.check:
        import onnxruntime as ort

        sess = ort.InferenceSession(str(out), providers=["CPUExecutionProvider"])
        run = lambda a, b: sess.run(["freq", "time"], {"mix": a, "spec": b})  # noqa: E731
        t0 = time.perf_counter()
        got = ref.separate_window(run, mix)
        t1 = time.perf_counter()
        with torch.no_grad():
            want = m(torch.from_numpy(mix[None]))[0].numpy()
        err = np.abs(got - want).max()
        print(f"max abs diff vs PyTorch: {err:.2e} (CPU ORT {t1 - t0:.2f}s)")
        if err > 1e-3:
            raise SystemExit("ONNX output does not match PyTorch")


if __name__ == "__main__":
    main()
