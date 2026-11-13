# What it takes to beat Krisp

RalpQuiet's immediate goal is better call audio. The full comparison also covers Krisp's meeting assistant, accent conversion and translation. Each row needs its own evidence; a single transcription score cannot settle the product comparison.

| Area | Test for a win | Next comparison |
| --- | --- | --- |
| Nearby speakers | Fewer competing words during silence **and** fewer lost target words during overlap, at equal listening quality and latency. | Compare the shipped VoxLock export, teachers and distilled models. Use LibriSpeech mixtures and independently recorded calls; use Krisp's labeled set only when permitted by its noncommercial terms. |
| Natural voice and everyday noise | Better paired listening scores for voice quality and residual noise; no regression on clean speech. | Compare full-band RNNoise, DeepFilterNet3, DPDFNet and FastEnhancer on EARS-WHAM v2 and real meetings. Score noise, voice distortion and overall quality independently. |
| Echo and room acoustics | Better echo rejection with local speech intact, including speaker movement and double talk; less audible room reverb. | Validate AEC3 against Krisp on actual Mac speakers, wired USB and Bluetooth. Separately compare dereverberation models using EARS-Reverb and impulse responses. |
| Call experience | Fewer dropouts and routing failures at a comparable end-to-end delay, measured through the browser boundary. | Test Chrome/Meet, Zoom and Teams. Record p95 and p99 latency, audible gaps, CPU, energy and recovery after disconnect/sleep; track startup and device switching. |
| Voice setup and privacy | Better rejection across voices and microphones, with a safer way to recover from a wrong target. | Compare explicit reference, auto-lock and hybrid paths. Test background-only starts, mismatched enrollment, similar voices, and switching. |
| Meeting assistant | Better transcript accuracy and notes with attributable decisions/action items; users can inspect, edit, export and delete them. | Compare a local transcription/diarization pipeline against human-labeled recordings. Measure language coverage, attribution, and factual errors before expanding to summaries. |
| Accent and translation | Higher intelligibility and meaning preservation without unwanted changes to voice identity. | Evaluate by language pair using bilingual raters before calling any feature comparable. |
| Distribution and trust | Installation succeeds on supported devices, and users can audit what leaves the machine. | Signed and notarized packages, update and rollback tests, offline operation, and explicit controls for retention. |

**Experimental design:** Use the same input signal per product (fresh model state per clip), the same playback level, randomized A/B order, and held-out speakers. Report paired confidence intervals and results split by clean speech, overlap, secondary-only, and echo rather than one score averaged across conditions. Where an audio SDK and desktop app behave differently, label and measure each independently. Include a clean passthrough control.

Research starting points: [Krisp Voice Isolation Benchmark](https://huggingface.co/datasets/Krisp-AI/VoiceIsolation-Benchmark-Dataset), [VoxLock](https://arxiv.org/abs/2610.40727), [Personalized PercepNet](https://arxiv.org/abs/2106.04129), [FastEnhancer](https://github.com/aask1357/fastenhancer), [DPDFNet](https://github.com/ceva-ip/DPDFNet), and [WebRTC Audio Processing](https://webrtc.googlesource.com/src/+/refs/heads/main/api/audio/audio_processing.h).
