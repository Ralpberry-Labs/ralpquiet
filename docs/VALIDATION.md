# Validation and release criteria

This is an early source preview. The A/B flow and the live path use the same model, but offline quality and routing reliability are separate claims.

## Reproducible tests

Local host: Apple M5 Pro, macOS 26.7.1. Use `.dist/rq-verify` for DSP sanity checks and `RQ_BENCH=1 ./scripts/benchmark.py` for the synthetic speech comparisons.

| Check | Result |
| --- | --- |
| Build and local code signatures | SwiftUI application and both Core Audio bundles compiled; plist and signature checks passed. |
| Offline ordinary noise smoke test | 60.6 dB RMS reduction on synthetic white noise. RNNoise processed 10 ms blocks in about 0.35–0.53 ms on this Mac. |
| Ordinary noise plus speech | +11.2 dB SI-SDR on one synthetic speech + white-noise mixture at equal RMS. |
| Competing speech while the target also speaks | +8.1 dB SI-SDR for VoxLock on an equal-RMS synthetic two-voice mixture, compared with −0.4 dB for RNNoise. |
| Competing speech while the target is quiet | 51.1 dB RMS reduction for VoxLock in this sample; approximately 0 dB for RNNoise. |
| Live speaker extraction throughput | The full-precision model averaged roughly 254 ms for each 80 ms block in a tested host session (three inference threads). That run failed the real-time requirement. The app checks local inference speed before starting this mode and stops the route if its bounded input buffer overruns. The ordinary noise mode does not depend on VoxLock. |
| End-to-end Meet call | Pending. The virtual devices were built and validated as bundles; loading them and completing a browser call require the local driver installation and restart. |

SI-SDR compares the output waveform to the known clean target without rewarding simple gain changes. These synthetic utterances and equal-RMS mixtures are a reproducible check, not a listening study. The model was not evaluated against Krisp's app on this machine. Throughput measurements exclude device buffers and do not represent end-to-end latency.

## Before calling this Krisp quality

- Run paired and level-matched blind listening tests across real home, office, and café recordings. Include clean speech, fan/keyboard noise, close competing speakers, same-timbre speakers, multiple talkers, device mismatch, and target-absent segments. Rate speech distortion, intelligibility, and leakage independently. When the dataset terms permit it, use the labeled Krisp benchmark for comparable ASR metrics.
- Test on representative Apple Silicon Macs under load. Measure actual delivery delay and callback gaps during a full call, changes in clock drift, sleeping and waking, and unplugging devices. A suggested isolation gate is sustainable throughput and no callback dropouts over 30 minutes.
- Test both directions in Chrome/Meet and with other supported calling apps. Confirm that stopping routing sends silence, the system default is not changed, the microphone permission flow works, and a local test plays via the selected physical speaker.
- Add dedicated acoustic echo cancellation and evaluate loudspeaker double talk before claiming echo parity. Use Developer ID signing and notarization before distributing a one-click installer.
