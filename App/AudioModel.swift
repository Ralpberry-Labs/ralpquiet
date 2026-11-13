/* SPDX-License-Identifier: GPL-3.0-or-later */
import AVFoundation
import CoreAudio
import Foundation
import Observation
import QuietEngine

struct AudioEndpoint: Identifiable, Equatable {
    let id: String
    let name: String

    static let mic = "RalpQuietMicrophone_UID"
    static let speaker = "RalpQuietSpeaker_UID"
    var virtual: Bool {
        let label = name.lowercased()
        return id.hasPrefix("RalpQuiet") || label.contains("krisp") || label.contains("blackhole")
            || label.contains("loopback") || label.contains("virtual")
    }

    static func list(input: Bool) -> [AudioEndpoint] {
        func decode(_ array: inout rq_device, uid: Bool) -> String {
            if uid { return withUnsafeBytes(of: &array.uid) { decodeBytes($0) } }
            return withUnsafeBytes(of: &array.name) { decodeBytes($0) }
        }
        var devices = Array(repeating: rq_device(), count: 128)
        let count = devices.withUnsafeMutableBufferPointer { rq_devices(input, $0.baseAddress, $0.count) }
        return devices.prefix(count).map {
            var device = $0
            return AudioEndpoint(id: decode(&device, uid: true), name: decode(&device, uid: false))
        }
    }

    static func defaultUID(input: Bool) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain
        )
        var device: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device) == noErr, device != 0 else { return nil }
        address.mSelector = kAudioDevicePropertyDeviceUID
        var uid: Unmanaged<CFString>?
        size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &uid) == noErr,
              let uid else { return nil }
        return uid.takeRetainedValue() as String
    }
}

private func decodeBytes(_ bytes: UnsafeRawBufferPointer) -> String {
    String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self)
}

@MainActor @Observable final class AudioModel {
    var microphones: [AudioEndpoint] = []
    var speakers: [AudioEndpoint] = []
    var micInstalled = false
    var speakerInstalled = false
    var microphone = ""
    var speaker = ""
    var suppressMic = true { didSet { rq_route_set_suppress(outgoing, suppressMic) } }
    var suppressSpeaker = true { didSet { rq_route_set_suppress(incoming, suppressSpeaker) } }
    var speakerEnabled = false
    var echoEnabled = true
    private var echo: OpaquePointer?
    var isolateVoice = false
    var hasProfile = false
    var enrolling = false
    var comparing = false
    private var voiceProfile: Data?
    private var enrollmentMicrophone = ""
    private var comparisonTask: Task<Void, Never>?
    private var modelsPath: String { Bundle.main.resourceURL!.appendingPathComponent("VoxLock").path }
    var live = false
    var pending = false
    var recording = false
    var stoppingTest = false
    var testProgress = 0.0
    var testing = false
    var inputLevel: Float = 0
    var outputLevel: Float = 0
    var audioGaps: UInt32 = 0
    var issue: String?
    var sampleSeconds = 0.0
    var playing: Clip?
    enum Clip: String { case original = "Original", filtered = "Cleaned" }
    private var original: Data?
    private var filtered: Data?
    private var player: AVAudioPlayer?
    private var outgoing: OpaquePointer?
    private var incoming: OpaquePointer?
    private var preview: OpaquePointer?
    private var lastRefresh = Date.distantPast
    private var monitorTask: Task<Void, Never>?

    init() { refreshDevices() }

    func startMonitoring() {
        guard monitorTask == nil else { return }
        monitorTask = Task { [weak self] in
            while !Task.isCancelled {
                guard self != nil else { return }
                self?.tick()
                try? await Task.sleep(for: .milliseconds(125))
            }
        }
    }

    func shutdown() {
        monitorTask?.cancel()
        monitorTask = nil
        stop()
        clearTest()
        eraseVoice()
    }

    func refreshDevices() {
        let allInputs = AudioEndpoint.list(input: true)
        let allOutputs = AudioEndpoint.list(input: false)
        micInstalled = allInputs.contains { $0.id == AudioEndpoint.mic }
            && allOutputs.contains { $0.id == AudioEndpoint.mic }
        speakerInstalled = allInputs.contains { $0.id == AudioEndpoint.speaker }
            && allOutputs.contains { $0.id == AudioEndpoint.speaker }
        microphones = allInputs.filter { !$0.virtual }
        speakers = allOutputs.filter { !$0.virtual }
        let activeMicLost = !microphones.contains(where: { $0.id == microphone })
        let activeSpeakerLost = !speakers.contains(where: { $0.id == speaker })
        if (live && (!micInstalled || activeMicLost || (speakerEnabled && (!speakerInstalled || activeSpeakerLost))))
            || (preview != nil && activeMicLost) {
            stop()
            issue = "An audio device disconnected. Select a device and start again."
        }
        if activeMicLost {
            let fallback = AudioEndpoint.defaultUID(input: true)
            if hasProfile { eraseVoice() }
            microphone = microphones.first(where: { $0.id == fallback })?.id ?? microphones.first?.id ?? ""
        }
        if activeSpeakerLost {
            let fallback = AudioEndpoint.defaultUID(input: false)
            speaker = speakers.first(where: { $0.id == fallback })?.id ?? speakers.first?.id ?? ""
        }
    }

    private func authorize() async -> Bool {
        let access: Bool
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: access = true
        case .notDetermined: access = await AVCaptureDevice.requestAccess(for: .audio)
        default: access = false
        }
        if !access { issue = "Allow RalpQuiet in System Settings → Privacy & Security → Microphone." }
        return access
    }

    private nonisolated static func createRoute(_ input: String, _ output: String?, suppress: Bool,
                                    models: String? = nil, profile: Data? = nil, echo: OpaquePointer? = nil, render: Bool = false) -> (OpaquePointer?, String?) {
        var error = Array(repeating: CChar(0), count: 256)
        func withOptional<T>(_ str: String?, _ body: (UnsafePointer<CChar>?) -> T) -> T {
            if let str { return str.withCString(body) }
            return body(nil)
        }
        let route = input.withCString { inputID in
            withOptional(output) { outputID in
                withOptional(models) { modelID in
                    error.withUnsafeMutableBufferPointer { err in
                        if let profile {
                            return profile.withUnsafeBytes { view in
                                let voice = view.bindMemory(to: UInt8.self)
                                return rq_route_open(inputID, outputID, suppress, modelID, voice.baseAddress, voice.count, echo, render, err.baseAddress, err.count)
                            }
                        }
                        return rq_route_open(inputID, outputID, suppress, modelID, nil, 0, echo, render, err.baseAddress, err.count)
                    }
                }
            }
        }
        return (route, route == nil ? error.withUnsafeBytes { decodeBytes($0) } : nil)
    }

    private func openRoute(_ input: String, _ output: String?, suppress: Bool, echo: OpaquePointer? = nil, render: Bool = false) -> OpaquePointer? {
        let (route, message) = Self.createRoute(input, output, suppress: suppress, echo: echo, render: render)
        if let message { issue = message }
        return route
    }

    private struct RouteResult: @unchecked Sendable {
        let pointer: OpaquePointer?
        let message: String?
    }

    private nonisolated static func createIsolationRoute(input: String, models: String, profile: Data, echoAddress: UInt?) -> RouteResult {
        let (route, message) = createRoute(input, AudioEndpoint.mic, suppress: false, models: models, profile: profile, echo: echoAddress.flatMap(OpaquePointer.init(bitPattern:)))
        return RouteResult(pointer: route, message: message)
    }

    func start() async {
        guard !live, !pending, !testing, !comparing else { return }
        guard micInstalled, !microphone.isEmpty else {
            issue = "Install the virtual microphone and select a physical input first."
            return
        }
        guard !speakerEnabled || (speakerInstalled && !speaker.isEmpty) else {
            issue = "Install the virtual speaker and select a physical output, or turn speaker routing off."
            return
        }
        issue = nil
        clearPlayback()
        pending = true
        defer { pending = false }
        guard await authorize() else { return }
        let nextEcho = speakerEnabled && echoEnabled ? rq_echo_open() : nil
        if speakerEnabled && echoEnabled {
            guard nextEcho != nil else { issue = "Could not initialize acoustic echo cancellation."; return }
        }
        defer { if !live { closeRoutes(); rq_echo_close(nextEcho) } }
        if isolateVoice {
            guard let profile = voiceProfile, microphone == enrollmentMicrophone else {
                issue = "Record a new voice sample with this microphone before turning on Only my voice."
                return
            }
            let input = microphone, models = modelsPath, echoAddress = nextEcho.map { UInt(bitPattern: $0) }
            let result = await Task.detached(priority: .userInitiated) {
                Self.createIsolationRoute(input: input, models: models, profile: profile, echoAddress: echoAddress)
            }.value
            guard let route = result.pointer else { issue = result.message; return }
            guard microphone == input, hasProfile, micInstalled else {
                rq_route_close(route)
                issue = "The microphone changed while preparing voice isolation. Select a microphone and try again."
                return
            }
            outgoing = route
        } else {
            guard let route = openRoute(microphone, AudioEndpoint.mic, suppress: suppressMic, echo: nextEcho) else { return }
            outgoing = route
        }
        if speakerEnabled {
            incoming = openRoute(AudioEndpoint.speaker, speaker, suppress: suppressSpeaker, echo: nextEcho, render: true)
            if incoming == nil { return }
        }
        echo = nextEcho
        live = true
    }

    func stop() {
        if testing {
            testing = false
            stoppingTest = false
            recording = false
            enrolling = false
        }
        closeRoutes()
    }

    private func closeRoutes() {
        rq_route_close(incoming); incoming = nil
        rq_route_close(outgoing); outgoing = nil
        rq_route_close(preview); preview = nil
        rq_echo_close(echo); echo = nil
        live = false
        inputLevel = 0
        outputLevel = 0
        audioGaps = 0
    }

    func runTest(enroll: Bool = false) async {
        guard !pending, !testing, !comparing, !microphone.isEmpty, !enroll || !live else { return }
        clearTest()
        issue = nil
        pending = true
        defer { pending = false }
        guard await authorize() else { return }
        if outgoing == nil { preview = openRoute(microphone, nil, suppress: suppressMic) }
        guard let route = outgoing ?? preview else { return }
        guard rq_test_start(route) else {
            rq_route_close(preview); preview = nil
            issue = "Could not allocate a test recording."
            return
        }
        enrolling = enroll
        testProgress = 0
        testing = true
        recording = true
    }

    func endTest() {
        guard testing else { return }
        recording = false
        stoppingTest = true
        _ = rq_test_stop(outgoing ?? preview)
    }

    func tick() {
        if Date().timeIntervalSince(lastRefresh) > 2 {
            lastRefresh = Date()
            refreshDevices()
        }
        let route = outgoing ?? preview
        let stats = rq_route_status(route)
        let back = rq_route_status(incoming)
        if stats.isolation_failed {
            stop()
            issue = "Voice isolation could not keep up and was stopped. Restart routing to try again."
        } else if route != nil && (!stats.running || (incoming != nil && !back.running)) {
            stop()
            issue = "The audio stream stopped. Check your devices and start again."
        }
        inputLevel = max(stats.input_peak, inputLevel * 0.7)
        outputLevel = max(stats.output_peak, outputLevel * 0.7)
        audioGaps = stats.underruns &+ stats.overruns &+ back.underruns &+ back.overruns
        if let route, testing, outgoing != nil || preview != nil {
            testProgress = Double(stats.recorded_frames) / Double(RQ_RATE * RQ_TEST_SECONDS)
            if stoppingTest || testProgress >= 1 {
                let count = Int(rq_test_stop(route))
                if count > 0 { finishTest(route, frames: count) }
                else if stoppingTest && stats.recorded_frames == 0 {
                    // A recording stopped before a complete frame contains no usable audio.
                    testing = false; stoppingTest = false; recording = false; enrolling = false
                    rq_route_close(preview); preview = nil
                }
            }
        }
        if let player, !player.isPlaying { clearPlayback() }
    }

    private func finishTest(_ route: OpaquePointer, frames: Int) {
        var raw = Array(repeating: Float(0), count: frames)
        var clean = raw
        rq_test_copy(route, &raw, &clean, UInt32(frames))
        let wasEnrolling = enrolling
        testing = false; stoppingTest = false; recording = false; enrolling = false
        rq_route_close(preview); preview = nil
        if wasEnrolling {
            saveVoice(raw)
        } else if frames >= Int(RQ_RATE / 4) {
            original = wav(raw)
            sampleSeconds = Double(frames) / Double(RQ_RATE)
            if isolateVoice, let profile = voiceProfile, microphone == enrollmentMicrophone {
                let path = modelsPath
                comparing = true
                comparisonTask = Task { [weak self] in
                    let output = await Task.detached(priority: .userInitiated) { [raw, path, profile, frames] () -> [Float]? in
                        var samples = Array(repeating: Float(0), count: frames)
                        return profile.withUnsafeBytes { view in
                            let voice = view.bindMemory(to: UInt8.self)
                            return rq_personalized_compare(path, voice.baseAddress, voice.count, raw, UInt32(frames), &samples) ? samples : nil
                        }
                    }.value
                    guard let self, !Task.isCancelled else { return }
                    self.comparing = false
                    self.comparisonTask = nil
                    if let output { self.filtered = self.wav(output) }
                    else {
                        self.clearTest()
                        self.issue = "Voice isolation could not process the test recording."
                    }
                }
            } else {
                filtered = wav(clean)
            }
        } else {
            issue = "The recording was too short. Try speaking for a few seconds before stopping."
        }
    }

    private func saveVoice(_ raw: [Float]) {
        let rate = Int(RQ_RATE)
        let voiced = stride(from: 0, to: raw.count - min(raw.count, 960), by: 960).filter { i in
            raw[i..<(i + 960)].reduce(0.0) { $0 + Double($1 * $1) } / 960 > 0.00004
        }.count * 960
        guard raw.count >= 4 * rate, voiced >= rate else {
            issue = "Record at least four seconds and speak clearly through the sample."
            return
        }
        voiceProfile = wav(raw)
        enrollmentMicrophone = microphone
        hasProfile = true
        isolateVoice = true
    }

    func eraseVoice() {
        voiceProfile = nil
        enrollmentMicrophone = ""
        hasProfile = false
        isolateVoice = false
    }

    func microphoneChanged() {
        if hasProfile, microphone != enrollmentMicrophone { eraseVoice() }
        clearTest()
    }

    func clearTest() {
        comparisonTask?.cancel()
        comparisonTask = nil
        comparing = false
        clearPlayback()
        original = nil
        filtered = nil
        sampleSeconds = 0
    }

    func play(_ clip: Clip) {
        if playing == clip { clearPlayback(); return }
        let position = player?.currentTime ?? 0
        clearPlayback()
        guard let sample = clip == .original ? original : filtered, !speaker.isEmpty else { return }
        do {
            let play = try AVAudioPlayer(data: sample)
            play.currentDevice = speaker
            play.currentTime = position
            guard play.play() else { issue = "Could not play the recording on the selected output."; return }
            player = play
            playing = clip
        } catch {
            issue = "Could not play the recording: \(error.localizedDescription)"
        }
    }

    private func clearPlayback() {
        player?.stop()
        player = nil
        playing = nil
    }

    private func wav(_ samples: [Float]) -> Data {
        var data = Data()
        data.reserveCapacity(44 + 2 * samples.count)
        func put<T: FixedWidthInteger>(_ value: T) {
            var little = value.littleEndian
            withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
        }
        let bytes = UInt32(samples.count * 2)
        data.append(contentsOf: "RIFF".utf8); put(bytes + 36)
        data.append(contentsOf: "WAVEfmt ".utf8); put(UInt32(16))
        put(UInt16(1)); put(UInt16(1)); put(UInt32(RQ_RATE)); put(UInt32(2 * RQ_RATE))
        put(UInt16(2)); put(UInt16(16))
        data.append(contentsOf: "data".utf8); put(bytes)
        for sample in samples {
            put(Int16((min(1, max(-1, sample)) * 32767).rounded()))
        }
        return data
    }
}
