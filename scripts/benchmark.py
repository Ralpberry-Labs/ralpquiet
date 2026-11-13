#!/usr/bin/env python3
"""Synthetic desktop smoke test. Requires macOS say, ffmpeg and a built RalpQuiet."""
from array import array
from math import log10, sqrt
from pathlib import Path
from random import Random
from subprocess import run
from tempfile import TemporaryDirectory

RATE = 48_000
ROOT = Path(__file__).resolve().parent.parent
VERIFY = ROOT / ".dist/rq-verify"
MODELS = ROOT / ".build/deps/voxlock"


def run_checked(*args):
    run(args, check=True)


def voice(directory, name, preset, text):
    src = directory / f"{name}.aiff"
    wav = directory / f"{name}.wav"
    raw = directory / f"{name}.f32"
    run_checked("say", "-v", preset, "-o", src, text)
    run_checked("ffmpeg", "-v", "error", "-y", "-i", src, "-ar", str(RATE), "-ac", "1", wav)
    run_checked("ffmpeg", "-v", "error", "-y", "-i", wav, "-f", "f32le", raw)
    return wav, read(raw)


def read(path):
    return array("f", path.read_bytes())


def rms(signal):
    return sqrt(sum(x*x for x in signal) / max(1, len(signal)))


def scaled(signal, level):
    factor = level / rms(signal)
    return array("f", (x * factor for x in signal))


def db(a, b):
    return 20 * log10(max(a, 1e-10) / max(b, 1e-10))


def sisdr(reference, estimate):
    energy = sum(x*x for x in reference)
    gain = sum(x*y for x, y in zip(reference, estimate)) / energy
    error = sum((y - gain*x)**2 for x, y in zip(reference, estimate))
    return 10 * log10(max(1e-20, gain*gain*energy) / max(1e-20, error))


def main():
    with TemporaryDirectory(prefix="ralpquiet-bench-") as tmp:
        d = Path(tmp)
        sample, _ = voice(d, "sample", "Samantha", "This is a short recording of my voice. I am speaking in a quiet room. Keep this voice when you hear two people speaking.")
        _, target = voice(d, "target", "Samantha", "The project is on schedule. Please send the changed budget on Thursday. We can review the figures before the meeting starts. I will bring a printed copy.")
        _, other = voice(d, "background", "Daniel", "Nobody could understand what the radio presenter said. The museum opens at noon, and all train tickets have to be reserved two weeks in advance. We should call today.")
        n = min(len(target), len(other))
        target, other = scaled(target[:n], 0.07), scaled(other[:n], 0.07)
        rest = 4 * RATE
        # Equal RMS speech overlap, then at least four seconds of only the competitor.
        competitor = other + other[:rest]
        mix = array("f", (a+b for a, b in zip(target, other))) + other[:rest]
        src, normal, isolated = (d / p for p in ("mix.f32", "normal.f32", "isolated.f32"))
        src.write_bytes(mix.tobytes())
        run_checked(VERIFY, "--raw", src, normal)
        run_checked(VERIFY, "--isolate", MODELS, sample, src, isolated)
        assert len(read(normal)) == len(read(isolated)) == len(mix)
        keep, stop = slice(RATE, n - RATE), slice(n + RATE, None)
        baseline = sisdr(target[keep], mix[keep])
        before = rms(competitor[stop])
        isolated_gain = None
        for name, item in [("RNNoise", normal), ("Only my voice", isolated)]:
            data = read(item)
            gain = sisdr(target[keep], data[keep]) - baseline
            reduction = db(before, rms(data[stop]))
            print(f"{name}: {gain:+.1f} dB SI-SDR change during overlap, {reduction:.1f} dB less competing-only RMS", flush=True)
            if item == isolated:
                isolated_gain = gain
        # Separate ordinary noise from talker rejection using an independent mix.
        rng = Random(42)
        fan = array("f", (rng.uniform(-1, 1) for _ in target))
        fan = scaled(fan, 0.07)
        noisy = array("f", (a+b for a, b in zip(target, fan)))
        src.write_bytes(noisy.tobytes())
        run_checked(VERIFY, "--raw", src, normal)
        gain = sisdr(target[keep], read(normal)[keep]) - sisdr(target[keep], noisy[keep])
        print(f"RNNoise: {gain:+.1f} dB SI-SDR change on speech with equal RMS white noise")
        if isolated_gain is None or isolated_gain < 1 or gain < 1:
            raise SystemExit("The synthetic quality smoke test did not improve both mixtures")


if __name__ == "__main__":
    main()
