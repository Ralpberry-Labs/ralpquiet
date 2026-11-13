# RalpQuiet

**Local voice cleanup for calls on macOS.** Pick a physical microphone in RalpQuiet and a virtual microphone in your calling app. Use **Remove microphone noise** for everyday sounds or **Only my voice** (experimental) to keep the voice from a short sample when someone else is talking nearby.

RalpQuiet is a GPL-3.0 source preview from Ralpberry Labs. It runs entirely on your Mac. There is no account, audio upload, telemetry, or meeting bot.

| Feature | What this preview does |
| --- | --- |
| Microphone | RNNoise suppresses non-speech noise on the outgoing route. |
| Only my voice | VoxLock extracts speech that matches the local sample. Use it when someone else is talking nearby. A bad sample, a similar voice, or difficult acoustics can still cause leaks or remove part of your speech. Live mode checks inference speed before starting; the tested Mac did not meet the real-time requirement in our first sustained test. |
| Speaker | Optional noise removal on incoming meeting audio; the cleaned signal plays on your physical speaker or headphones. This uses a mono speech mix. |
| Run a test | Record up to 12 seconds, then switch between the original and cleaned version at the same playback position. Clips stay in memory. |

## Setup

Requires macOS 14 or later, Xcode Command Line Tools with Swift 6, and an administrator account for virtual driver installation. The developer build signs the bundles locally. The first build downloads pinned open-source dependencies and model weights; audio processing requires no network.

```sh
git clone https://github.com/Ralpberry-Labs/ralpquiet.git
cd ralpquiet
./scripts/build
./scripts/install-drivers
```

Restart the Mac after installing the drivers. Run `open .dist/RalpQuiet.app` and choose your physical microphone. You can **Run a test** without installing the drivers. Allow microphone access when macOS asks.

1. For **Only my voice**, choose **Record my voice** and speak alone for 5–12 seconds using your usual microphone. Test with someone speaking over you before relying on it for a call. The sample is scoped to this app session; use **Forget voice** to clear it.
2. Choose **Start routing**. In Google Meet, open **Settings → Audio** and select **RalpQuiet Microphone**. The status in RalpQuiet reports its own audio routing; confirm the device selection in Meet.
3. Leave Meet’s speaker set to your usual output for microphone-only use, or enable **Route meeting audio through RalpQuiet Speaker** before starting and select **RalpQuiet Speaker** in Meet.
4. For a fair A/B test, turn off Meet’s separate Noise cancellation setting if your account exposes it. Keep headphones connected if speaker bleed is an issue.
5. Switch Meet back to your usual devices before quitting RalpQuiet.

Run `./scripts/uninstall-drivers` and restart the Mac to remove the two virtual devices.

## Verification

```sh
.dist/rq-verify
RQ_BENCH=1 ./scripts/benchmark.py  # requires ffmpeg and the Samantha and Daniel macOS voices
```

See [test results and release criteria](docs/VALIDATION.md), [Krisp research and feature comparison](docs/KRISP-RESEARCH.md), and [third-party licenses](docs/THIRD-PARTY.md). This is an independent implementation. The preview does not include dedicated echo cancellation, automatic per-app audio detection, notes, transcription, or accent conversion.
