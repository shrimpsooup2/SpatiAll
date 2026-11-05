"""Write a real-music window + reference stems (NumPy STFT + CPU ONNX Runtime) for spatiall_selftest.

Usage: python tools/make_selftest_data.py song.mp3 models/htdemucs_4s.onnx OUTDIR
Writes OUTDIR/mix.f32 (planar [2][N]) and OUTDIR/stems.f32 (planar [S][2][N]), little-endian float32.
"""

import pathlib
import sys

import numpy as np
import onnxruntime as ort

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import htdemucs_ref as ref  # noqa: E402
from eval_streaming import load_audio  # noqa: E402


def main():
    song, model, outdir = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    sess = ort.InferenceSession(model, providers=["CPUExecutionProvider"])
    n = sess.get_inputs()[0].shape[-1]
    mix = load_audio(song, n / ref.SR + 0.1, 45)[:, :n].astype(np.float32)
    stems = ref.separate_window(lambda a, b: sess.run(["freq", "time"], {"mix": a, "spec": b}), mix)
    mix.tofile(outdir / "mix.f32")
    stems.astype(np.float32).tofile(outdir / "stems.f32")
    print(f"wrote {n} samples, {stems.shape[0]} stems to {outdir}")


if __name__ == "__main__":
    main()
