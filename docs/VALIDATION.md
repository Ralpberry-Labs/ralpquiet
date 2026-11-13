# Validation and release criteria

The following results compare RalpQuiet revisions. No results are from a head-to-head listening test with the Krisp desktop app. Local host: Apple M5 Pro, macOS 26.7.1.

| Check | Earlier preview | Native VoxLock v1.3 + WebRTC AEC3 |
| --- | --- | --- |
| Warm processing throughput | 254 ms per 80 ms of audio (3.18× the block duration) | 21–29 ms per 40 ms of audio (0.53–0.73× the block duration) across local runs. Individual blocks can take longer than 40 ms. |
| Call-paced synthetic replay | Could not keep up | Accepted 62.49 s at 48 kHz in 10 ms packets, with zero underruns after priming. First buffered output at 200 ms. A separate 12.49 s run delivered zero underruns after priming at 190 ms. These are worker tests, excluding device, driver and browser latency. |
| Equal-level synthetic two-talker mixture | +8.1 dB SI-SDR | +14.0 dB SI-SDR. On the same mixture RNNoise alone scored −0.4 dB. |
| Competing voice alone | 51.1 dB RMS reduction | 50.8 dB RMS reduction; approximately 0 dB for RNNoise alone. |
| Additional synthetic mixtures | Not measured | +17.7 dB SI-SDR with competitor 6 dB louder; +7.8 dB for the same-gender pair. |
| Ordinary noise plus speech | +11.2 dB SI-SDR | +11.2 dB SI-SDR for equal RMS white noise; this mode still uses RNNoise. |
| Acoustic echo | No AEC | AEC3 removed 37.1 dB of synthetic echo after convergence; while an independent local tone played alongside the echo, its level changed by −1.0 dB. |
| Mac application | Built and verified locally | SwiftUI application and drivers build; local signature and plist checks pass. |
| End-to-end browser call | Pending | Pending. Requires loading both drivers and testing real hardware. |

SI-SDR rewards preservation of the known clean target waveform after gain alignment. These are synthetic fixtures, not a claim about real call quality. The pace test uses the same inference worker, queues and priming threshold as live mode. It does not include Core Audio or measure the total delay heard by a participant. The 100 ms priming reserve absorbs jitter, but gives up latency. In an initial run using a smaller reserve, 12.49 seconds of input produced two underruns.

```sh
.dist/rq-verify
.dist/rq-verify --echo
RQ_BENCH=1 ./scripts/benchmark.py --paced
RQ_BENCH=1 ./scripts/benchmark.py --soak
```

The last two commands synthesize fresh fixtures with macOS voices and use ffmpeg for conversion. The loopback test uses a delayed, attenuated two-tap echo path and an independent near-end tone. For real double talk, use actual speakers and a separate near-end talker.

See [what it takes to beat Krisp](HEAD-TO-HEAD.md) for the criteria and datasets to validate the product across different users and call conditions.
