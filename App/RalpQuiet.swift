/* SPDX-License-Identifier: GPL-3.0-or-later */
import SwiftUI

@main struct RalpQuiet: App {
    @State private var audio = AudioModel()

    var body: some Scene {
        Window("RalpQuiet", id: "main") {
            Dashboard(audio: audio)
                .frame(minWidth: 544, minHeight: 640)
                .onAppear { audio.startMonitoring() }
        }
        .defaultSize(width: 580, height: 760)
        MenuBarExtra("RalpQuiet", systemImage: audio.live ? "waveform.circle.fill" : "waveform.circle") {
            ShowDashboardButton()
            if audio.live { Button("Stop routing") { audio.stop() } }
            Divider()
            Button("Quit RalpQuiet") { audio.shutdown(); NSApp.terminate(nil) }.keyboardShortcut("q")
        }
        .commandsRemoved()
    }
}

private struct ShowDashboardButton: View {
    @Environment(\.openWindow) private var openWindow
    var body: some View {
        Button("Show RalpQuiet") {
            openWindow(id: "main")
            NSApp.activate(ignoringOtherApps: true)
        }
    }
}

struct Dashboard: View {
    @Bindable var audio: AudioModel
    @State private var showSetup = false
    private let mint = Color(red: 0.11, green: 0.66, blue: 0.49)
    private var locked: Bool { audio.live || audio.testing || audio.pending || audio.comparing }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 17) {
                header
                setupBanner
                VStack(alignment: .leading, spacing: 14) {
                    Label("Audio devices", systemImage: "slider.horizontal.3").font(.headline)
                    devicePicker("Physical microphone", icon: "mic.fill", devices: audio.microphones, selection: $audio.microphone)
                    Toggle("Remove microphone noise", isOn: $audio.suppressMic).disabled(audio.isolateVoice)
                    Divider()
                    devicePicker("Physical speaker", icon: "speaker.wave.2.fill", devices: audio.speakers, selection: $audio.speaker)
                    Toggle("Route meeting audio through RalpQuiet Speaker", isOn: $audio.speakerEnabled).disabled(locked)
                    if audio.speakerEnabled { Toggle("Remove noise from meeting audio", isOn: $audio.suppressSpeaker) }
                }.padding(17).quietPanel()
                voicePanel
                testPanel
                if let issue = audio.issue {
                    Label(issue, systemImage: "exclamationmark.circle.fill")
                        .font(.callout).foregroundStyle(.orange)
                        .accessibilityAddTraits(.updatesFrequently)
                }
                HStack {
                    Label("On-device audio", systemImage: "lock.shield").foregroundStyle(.secondary)
                    Spacer()
                    if audio.live && audio.audioGaps > 0 {
                        Text("\(audio.audioGaps) audio buffer events").foregroundStyle(.secondary)
                    }
                    Text("Ralpberry Labs · Preview").foregroundStyle(.secondary)
                }.font(.caption)
            }
            .padding(22)
        }
        .tint(mint)
        .sheet(isPresented: $showSetup) { setupSheet }
        .onChange(of: audio.microphone) { _, _ in audio.microphoneChanged() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
            audio.shutdown()
        }
    }

    private var header: some View {
        HStack(spacing: 13) {
            Image(systemName: "waveform")
                .font(.system(size: 23, weight: .semibold))
                .foregroundStyle(mint)
                .frame(width: 48, height: 48)
                .background(mint.opacity(0.12), in: RoundedRectangle(cornerRadius: 13))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text("RalpQuiet").font(.system(size: 29, weight: .bold))
                Text("Noise cancellation for calls").foregroundStyle(.secondary)
            }
            Spacer()
            Button(audio.live ? "Stop routing" : "Start routing") {
                if audio.live { audio.stop() }
                else { Task { await audio.start() } }
            }
            .buttonStyle(.borderedProminent).controlSize(.large)
            .disabled(audio.pending || (!audio.live && (audio.testing || audio.comparing || !audio.micInstalled || audio.microphone.isEmpty)))
        }
    }

    private var setupBanner: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 11) {
                Image(systemName: audio.live ? "checkmark.circle.fill" : "video.badge.waveform")
                    .font(.title3).foregroundStyle(mint).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 4) {
                    Text(audio.live ? "Audio routing is live" : audio.micInstalled ? "Connect RalpQuiet to Google Meet" : "Set up your virtual microphone")
                        .font(.headline)
                    Text(audio.micInstalled
                         ? "In Meet’s Audio settings, choose RalpQuiet Microphone. \(audio.speakerEnabled ? "Choose RalpQuiet Speaker for output." : "Keep your regular speaker for output.")"
                         : "Install the two virtual audio devices to use RalpQuiet in Meet, Zoom, or Teams. You can run a local test now.")
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 4)
                Button("Setup") { showSetup = true }
            }
            if audio.live {
                HStack(spacing: 16) {
                    meter("Input", level: audio.inputLevel)
                    meter("Cleaned", level: audio.outputLevel)
                }.padding(.leading, 28)
            }
        }
        .padding(16)
        .background(mint.opacity(0.09), in: RoundedRectangle(cornerRadius: 14))
    }

    private func meter(_ label: String, level: Float) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(label).font(.caption).foregroundStyle(.secondary)
            ProgressView(value: min(1, max(0, (20 * log10(Double(max(level, 0.001))) + 54) / 54)))
                .accessibilityLabel(label + " level")
        }.frame(maxWidth: .infinity)
    }

    private func devicePicker(_ name: String, icon: String, devices: [AudioEndpoint], selection: Binding<String>) -> some View {
        Picker(selection: selection) {
            if devices.isEmpty { Text("No device found").tag("") }
            ForEach(devices) { Text($0.name).tag($0.id) }
        } label: { Label(name, systemImage: icon) }
        .disabled(locked).pickerStyle(.menu)
    }

    private var voicePanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label("Only my voice", systemImage: "person.wave.2").font(.headline)
                Text("Experimental").font(.caption).foregroundStyle(.secondary)
                    .padding(.horizontal, 7).padding(.vertical, 3)
                    .background(.quaternary, in: Capsule())
            }
            Text("Record 5–12 seconds alone using your call mic. Try a local test first; live isolation requires enough processing speed.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if audio.hasProfile {
                Toggle("Suppress other voices on the microphone", isOn: $audio.isolateVoice).disabled(locked)
            }
            if audio.enrolling {
                meter("Voice level", level: audio.inputLevel)
                ProgressView(value: audio.testProgress).accessibilityLabel("Voice sample progress")
                Button("Use this sample") { audio.endTest() }.disabled(audio.stoppingTest || audio.testProgress < 0.35)
            } else {
                HStack {
                    Button(audio.hasProfile ? "Replace sample" : "Record my voice") { Task { await audio.runTest(enroll: true) } }
                        .disabled(locked || audio.microphone.isEmpty)
                    if audio.hasProfile {
                        Button("Forget voice") { audio.eraseVoice(); audio.clearTest() }.disabled(locked)
                    }
                }
            }
            Text("The sample stays on this Mac for this session. Use Forget voice to clear it.")
                .font(.caption).foregroundStyle(.secondary)
        }.padding(17).quietPanel()
    }

    private var testPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label("Hear the difference", systemImage: "headphones").font(.headline)
            Text("Speak normally with your usual background noise. Compare the same recording with and without filtering. Headphones work best during playback.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if audio.comparing {
                ProgressView("Preparing cleaned audio…").controlSize(.small)
            } else if audio.testing && !audio.enrolling {
                meter("Microphone level", level: audio.inputLevel)
                ProgressView(value: audio.testProgress)
                    .accessibilityLabel("Recording progress")
                Button("Stop recording") { audio.endTest() }
                    .disabled(audio.stoppingTest)
            } else {
                HStack {
                    Button(audio.sampleSeconds > 0 ? "Record again" : "Run a test") { Task { await audio.runTest() } }
                        .disabled(audio.pending || audio.enrolling || audio.microphone.isEmpty)
                    if audio.sampleSeconds > 0 {
                        ForEach([AudioModel.Clip.original, .filtered], id: \.self) { clip in
                            Button { audio.play(clip) } label: {
                                Label(clip.rawValue, systemImage: audio.playing == clip ? "stop.fill" : "play.fill")
                            }.disabled(audio.speaker.isEmpty)
                        }
                        Spacer()
                        Button("Clear") { audio.clearTest() }
                    }
                }
            }
            Text("The recording is kept in memory until you clear it or quit.").font(.caption).foregroundStyle(.secondary)
        }.padding(17).quietPanel()
    }

    private var setupSheet: some View {
        VStack(alignment: .leading, spacing: 15) {
            Label("Connect your call", systemImage: "video").font(.title2.bold())
            Text("1. Install the RalpQuiet drivers using the command in the project’s setup guide, then restart your Mac.")
            Text("2. In RalpQuiet, select your physical microphone, then choose Start routing.")
            Text("3. In Google Meet, open Settings → Audio. Select RalpQuiet Microphone as the microphone. For incoming noise removal, enable speaker routing here and select RalpQuiet Speaker in Meet.")
            Text("Switch Meet back to your usual devices before quitting RalpQuiet.").foregroundStyle(.secondary)
            HStack {
                Link("Open setup guide", destination: URL(string: "https://github.com/Ralpberry-Labs/ralpquiet#setup")!)
                Spacer()
                Button("Done") { audio.refreshDevices(); showSetup = false }.keyboardShortcut(.defaultAction)
            }
        }.padding(24).frame(width: 465).fixedSize(horizontal: false, vertical: true)
    }
}

private extension View {
    func quietPanel() -> some View {
        self.frame(maxWidth: .infinity, alignment: .leading)
            .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(.quaternary, lineWidth: 1))
    }
}
