//
// DriverManager.swift
// AES67 Manager - Build #12
// Main interface for managing the AES67 driver
//

import Foundation
import SwiftUI
import CoreAudio

// MARK: - Pending Stream for Sample Rate Mismatch

struct PendingStreamInfo {
    let name: String
    let multicastIP: String
    let port: UInt16
    let numChannels: UInt16
    let sampleRate: UInt32
    let encoding: String
}

// MARK: - PTP Diagnostics

/// Swift representation of PTP diagnostic information
/// Not yet connected to the driver (the view shows sample values)
struct PTPDiagnostics {
    // Connection status
    var isConnected: Bool = false
    var isLocked: Bool = false
    var masterClockID: String? = nil
    var clockClass: Int = 248
    var clockAccuracy: Int = 254
    var offsetNs: Int64 = 0

    // Network diagnostics
    var firewallBlockingPTP: Bool = false
    var firewallBlockingRTP: Bool = false
    var lastMessageReceived: Int = -1
    var lastMessageTime: Date = Date()

    // Quality metrics
    var currentOffset: Double = 0.0
    var meanOffset: Double = 0.0
    var offsetStdDev: Double = 0.0
    var frequencyOffset: Double = 0.0

    // Timing quality
    var syncMessagesReceived: Int = 0
    var followUpMessagesReceived: Int = 0
    var delayReqMessagesSent: Int = 0
    var delayRespMessagesReceived: Int = 0
    var announceMessagesReceived: Int = 0

    // Error counters
    var stateTransitions: Int = 0
    var ignoredAnnounce: Int = 0
    var domainMismatchErrors: Int = 0

    // PTP domain info
    var currentDomain: Int = 0
    var preferredDomain: Int = 0
}

class DriverManager: ObservableObject {
    @Published var streams: [StreamInfo] = []
    @Published var isDriverLoaded: Bool = false
    @Published var showAddStreamSheet: Bool = false
    @Published var totalChannelsUsed: Int = 0

    // Sample rate management
    @Published var currentDeviceSampleRate: Double = 48000.0
    @Published var showSampleRateMismatchAlert: Bool = false
    @Published var pendingStream: PendingStreamInfo?

    // Supported sample rates for the device
    static let supportedSampleRates: [Double] = [44100, 48000, 88200, 96000, 176400, 192000]

    private let configURL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Library/Application Support/AES67Driver/config.json")

    private var refreshTimer: Timer?
    private var currentRefreshInterval: Double = 1.0

    init() {
        currentRefreshInterval = UserDefaults.standard.double(forKey: "refreshInterval")
        if currentRefreshInterval < 0.5 { currentRefreshInterval = 1.0 }
        checkDriverStatus()
        loadConfiguration()
        loadDeviceSampleRate()
        startAutoRefresh()
    }

    // MARK: - Driver Status

    func checkDriverStatus() {
        // Check if AES67Driver.driver exists in HAL plugins
        let driverPath = "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
        isDriverLoaded = FileManager.default.fileExists(atPath: driverPath)
    }

    func checkDriverInstallation() {
        checkDriverStatus()

        if isDriverLoaded {
            showAlert(title: "Driver Installed",
                     message: "AES67 driver is properly installed at /Library/Audio/Plug-Ins/HAL/")
        } else {
            showAlert(title: "Driver Not Found",
                     message: "Please install the AES67 driver package first.")
        }
    }

    func restartCoreAudio() {
        let task = Process()
        task.launchPath = "/bin/launchctl"
        task.arguments = ["kickstart", "-kp", "system/com.apple.audio.coreaudiod"]

        do {
            try task.run()
            task.waitUntilExit()

            if task.terminationStatus == 0 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                    self.checkDriverStatus()
                }
            } else {
                // Fall back to AppleScript with privilege escalation
                restartCoreAudioWithPrivileges()
            }
        } catch {
            restartCoreAudioWithPrivileges()
        }
    }

    private func restartCoreAudioWithPrivileges() {
        let script = NSAppleScript(source: """
            do shell script "launchctl kickstart -kp system/com.apple.audio.coreaudiod" with administrator privileges
            """)
        var error: NSDictionary?
        script?.executeAndReturnError(&error)

        if let error = error {
            DispatchQueue.main.async {
                self.showAlert(title: "Restart Failed",
                             message: "Could not restart Core Audio: \(error[NSAppleScript.errorMessage] ?? "Unknown error")")
            }
        } else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                self.checkDriverStatus()
            }
        }
    }

    // MARK: - Stream Management

    /// Result of attempting to add a stream
    enum AddStreamResult {
        case success
        case sampleRateMismatch(streamRate: UInt32, deviceRate: Double)
        case channelLimitExceeded
    }

    /// Adds a stream, checking for sample rate compatibility
    /// Returns the result indicating success or the reason for failure
    @discardableResult
    func addStream(name: String, multicastIP: String, port: UInt16,
                   numChannels: UInt16, sampleRate: UInt32, encoding: String,
                   ttl: UInt8 = 32, ptpDomain: Int = 0, description: String? = nil,
                   bypassSampleRateCheck: Bool = false) -> AddStreamResult {

        // Check sample rate compatibility unless bypassed
        if !bypassSampleRateCheck && sampleRate != UInt32(currentDeviceSampleRate) {
            // Store pending stream for later addition
            pendingStream = PendingStreamInfo(
                name: name,
                multicastIP: multicastIP,
                port: port,
                numChannels: numChannels,
                sampleRate: sampleRate,
                encoding: encoding
            )
            showSampleRateMismatchAlert = true
            return .sampleRateMismatch(streamRate: sampleRate, deviceRate: currentDeviceSampleRate)
        }

        // Check channel limit
        if totalChannelsUsed + Int(numChannels) > 128 {
            return .channelLimitExceeded
        }

        let stream = StreamInfo(
            id: UUID(),
            name: name,
            description: description,
            multicastIP: multicastIP,
            port: port,
            ttl: ttl,
            encoding: encoding,
            sampleRate: sampleRate,
            numChannels: numChannels,
            ptpDomain: ptpDomain
        )

        // Auto-assign device channels
        let deviceChannelStart = UInt16(totalChannelsUsed)

        let mapping = ChannelMappingInfo(
            streamID: stream.id,
            streamName: name,
            streamChannelCount: numChannels,
            deviceChannelStart: deviceChannelStart,
            deviceChannelCount: numChannels
        )

        var newStream = stream
        newStream.mapping = mapping

        streams.append(newStream)
        updateTotalChannels()
        saveConfiguration()

        return .success
    }

    /// Adds the pending stream after sample rate change confirmation
    func addPendingStream() {
        guard let pending = pendingStream else { return }

        _ = addStream(
            name: pending.name,
            multicastIP: pending.multicastIP,
            port: pending.port,
            numChannels: pending.numChannels,
            sampleRate: pending.sampleRate,
            encoding: pending.encoding,
            bypassSampleRateCheck: true
        )

        pendingStream = nil
    }

    /// Cancels the pending stream addition
    func cancelPendingStream() {
        pendingStream = nil
        showSampleRateMismatchAlert = false
    }

    func removeStream(_ stream: StreamInfo) {
        streams.removeAll { $0.id == stream.id }
        updateTotalChannels()
        saveConfiguration()
    }

    func refreshStatus() {
        checkDriverStatus()
        // In a real implementation, this would query the driver for current status
        // For now, we just refresh the driver status
    }

    // MARK: - Channel Mapping

    func assignMapping(streamID: UUID, deviceChannelStart: UInt16, deviceChannelCount: UInt16) {
        guard let index = streams.firstIndex(where: { $0.id == streamID }) else { return }

        let mapping = ChannelMappingInfo(
            streamID: streamID,
            streamName: streams[index].name,
            streamChannelCount: streams[index].numChannels,
            deviceChannelStart: deviceChannelStart,
            deviceChannelCount: deviceChannelCount
        )

        streams[index].mapping = mapping
        saveConfiguration()
    }

    func clearMapping(streamID: UUID) {
        guard let index = streams.firstIndex(where: { $0.id == streamID }) else { return }
        streams[index].mapping = nil
        saveConfiguration()
    }

    // MARK: - SDP Import/Export

    func importSDPFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "sdp")!]
        panel.allowsMultipleSelection = false
        panel.message = "Select an SDP file to import"

        panel.begin { response in
            if response == .OK, let url = panel.url {
                self.parseSDP(from: url)
            }
        }
    }

    /// Imports an SDP file directly from a URL (used for drag-and-drop)
    func importSDPFromURL(_ url: URL) {
        parseSDP(from: url)
    }

    func exportSDP(for stream: StreamInfo) {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "sdp")!]
        panel.nameFieldStringValue = "\(stream.name).sdp"
        panel.message = "Export SDP file"

        panel.begin { response in
            if response == .OK, let url = panel.url {
                self.generateSDP(for: stream, to: url)
            }
        }
    }

    /// Parses an SDP file and adds the stream.
    /// Uses rtpmap-based parsing for accurate codec/channel extraction.
    private func parseSDP(from url: URL) {
        do {
            let content = try String(contentsOf: url, encoding: .utf8)
            let lines = content.components(separatedBy: .newlines)
            var name = url.deletingPathExtension().lastPathComponent
            var multicastIP = ""
            var port: UInt16 = 5004
            var numChannels: UInt16 = 2
            var sampleRate: UInt32 = 48000
            var encoding = "L24"
            var ttl: UInt8 = 32
            var ptpDomain: Int = 0

            for line in lines {
                let trimmed = line.trimmingCharacters(in: .whitespaces)
                guard !trimmed.isEmpty else { continue }

                if trimmed.hasPrefix("s=") {
                    let sessionName = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                    if !sessionName.isEmpty && sessionName != "-" {
                        name = sessionName
                    }
                } else if trimmed.hasPrefix("c=") {
                    let parts = trimmed.components(separatedBy: .whitespaces).filter { !$0.isEmpty }
                    if parts.count >= 3 {
                        let addrParts = parts[2].components(separatedBy: "/")
                        multicastIP = addrParts[0]
                        if addrParts.count >= 2, let parsedTTL = UInt8(addrParts[1]) {
                            ttl = parsedTTL
                        }
                    }
                } else if trimmed.hasPrefix("m=audio ") {
                    let parts = trimmed.dropFirst(8).components(separatedBy: .whitespaces).filter { !$0.isEmpty }
                    if let portStr = parts.first?.components(separatedBy: "/").first,
                       let parsedPort = UInt16(portStr), parsedPort > 0 {
                        port = parsedPort
                    }
                } else if trimmed.contains("rtpmap") {
                    if let match = trimmed.range(of: #"(L\d+|AM824)/(\d+)(?:/(\d+))?"#, options: .regularExpression) {
                        let rtpInfo = String(trimmed[match])
                        let parts = rtpInfo.components(separatedBy: "/")
                        if parts.count >= 1 { encoding = parts[0] }
                        if parts.count >= 2, let r = UInt32(parts[1]) { sampleRate = r }
                        if parts.count >= 3, let c = UInt16(parts[2]) { numChannels = c }
                    }
                } else if trimmed.contains("clock-domain:") || trimmed.contains("ptp-domain") {
                    if let match = trimmed.range(of: #"\d+"#, options: .regularExpression) {
                        if let d = Int(trimmed[match]) { ptpDomain = d }
                    }
                }
            }

            guard !multicastIP.isEmpty else {
                print("Failed to import SDP: no multicast address found")
                return
            }

            addStream(name: name, multicastIP: multicastIP, port: port,
                     numChannels: numChannels, sampleRate: sampleRate, encoding: encoding,
                     ttl: ttl, ptpDomain: ptpDomain)

        } catch {
            print("Failed to import SDP: \(error)")
        }
    }

    private func generateSDP(for stream: StreamInfo, to url: URL) {
        var sdp = "v=0\n"
        sdp += "o=- \(Int(Date().timeIntervalSince1970)) 1 IN IP4 127.0.0.1\n"
        sdp += "s=\(stream.name)\n"
        sdp += "c=IN IP4 \(stream.multicastIP)/\(stream.ttl)\n"
        sdp += "t=0 0\n"
        sdp += "m=audio \(stream.port) RTP/AVP \(stream.payloadType)\n"
        sdp += "a=rtpmap:\(stream.payloadType) \(stream.encoding)/\(stream.sampleRate)/\(stream.numChannels)\n"
        sdp += "a=ptime:1\n"

        do {
            try sdp.write(to: url, atomically: true, encoding: .utf8)
        } catch {
            print("Failed to export SDP: \(error)")
        }
    }

    // MARK: - Configuration Persistence

    private func loadConfiguration() {
        guard FileManager.default.fileExists(atPath: configURL.path) else { return }

        do {
            let data = try Data(contentsOf: configURL)
            let config = try JSONDecoder().decode(DriverConfiguration.self, from: data)

            // Convert config to streams
            streams = config.streams.compactMap { streamConfig in
                let id = UUID(uuidString: streamConfig.id) ?? UUID()
                let mapping = config.mappings.first { $0.streamID == id }

                return StreamInfo(
                    id: id,
                    name: streamConfig.name,
                    description: streamConfig.description,
                    multicastIP: streamConfig.multicastIP,
                    port: streamConfig.port,
                    encoding: streamConfig.encoding,
                    sampleRate: streamConfig.sampleRate,
                    numChannels: streamConfig.numChannels,
                    payloadType: streamConfig.payloadType,
                    ptpDomain: streamConfig.ptpDomain,
                    mapping: mapping
                )
            }

            updateTotalChannels()
        } catch {
            print("Failed to load configuration: \(error)")
        }
    }

    private func saveConfiguration() {
        // Create config directory if needed
        let configDir = configURL.deletingLastPathComponent()
        try? FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)

        let config = DriverConfiguration(
            streams: streams.map { stream in
                StreamConfig(
                    id: stream.id.uuidString,
                    name: stream.name,
                    description: stream.description,
                    multicastIP: stream.multicastIP,
                    port: stream.port,
                    encoding: stream.encoding,
                    sampleRate: stream.sampleRate,
                    numChannels: stream.numChannels,
                    payloadType: stream.payloadType,
                    ptpDomain: stream.ptpDomain
                )
            },
            mappings: streams.compactMap { $0.mapping }
        )

        do {
            let encoder = JSONEncoder()
            encoder.outputFormatting = .prettyPrinted
            let data = try encoder.encode(config)
            try data.write(to: configURL, options: .atomic)
        } catch {
            print("Failed to save configuration: \(error)")
        }
    }

    // MARK: - Helpers

    private func updateTotalChannels() {
        totalChannelsUsed = streams.reduce(0) { $0 + Int($1.numChannels) }
    }

    private func startAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: currentRefreshInterval, repeats: true) { [weak self] _ in
            self?.refreshStatus()
        }
    }

    /// Updates the auto-refresh interval (called when user changes setting)
    func updateRefreshInterval(_ interval: Double) {
        guard interval >= 0.5, interval != currentRefreshInterval else { return }
        currentRefreshInterval = interval
        startAutoRefresh()
    }

    private func showAlert(title: String, message: String) {
        DispatchQueue.main.async {
            let alert = NSAlert()
            alert.messageText = title
            alert.informativeText = message
            alert.alertStyle = .informational
            alert.runModal()
        }
    }

    // MARK: - Sample Rate Management

    /// Loads the current device sample rate from CoreAudio
    private func loadDeviceSampleRate() {
        guard let deviceID = findAES67DeviceID() else {
            // Fall back to default if device not found
            currentDeviceSampleRate = 48000.0
            return
        }

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var sampleRate: Float64 = 48000.0
        var dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &sampleRate
        )

        if status == noErr {
            currentDeviceSampleRate = sampleRate
        } else {
            print("Failed to get device sample rate: \(status)")
            currentDeviceSampleRate = 48000.0
        }
    }

    /// Sets the device sample rate
    /// - Parameter rate: The target sample rate in Hz
    /// - Returns: true if successful, false otherwise
    @discardableResult
    func setDeviceSampleRate(_ rate: Double) -> Bool {
        guard Self.supportedSampleRates.contains(rate) else {
            print("Unsupported sample rate: \(rate)")
            return false
        }

        guard let deviceID = findAES67DeviceID() else {
            print("AES67 device not found")
            return false
        }

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        // Check if property is settable
        var isSettable: DarwinBoolean = false
        let settableStatus = AudioObjectIsPropertySettable(deviceID, &propertyAddress, &isSettable)

        if settableStatus != noErr || !isSettable.boolValue {
            print("Sample rate property is not settable")
            return false
        }

        var sampleRate = Float64(rate)
        let dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectSetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            dataSize,
            &sampleRate
        )

        if status == noErr {
            currentDeviceSampleRate = rate
            print("Device sample rate changed to \(Int(rate)) Hz")
            return true
        } else {
            print("Failed to set device sample rate: \(status)")
            return false
        }
    }

    /// Changes device sample rate and adds the pending stream
    func changeSampleRateAndAddPendingStream() {
        guard let pending = pendingStream else { return }

        if setDeviceSampleRate(Double(pending.sampleRate)) {
            addPendingStream()
        }

        showSampleRateMismatchAlert = false
    }

    /// Finds the AES67 virtual audio device ID
    private func findAES67DeviceID() -> AudioDeviceID? {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        guard status == noErr else { return nil }

        let deviceCount = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: deviceCount)

        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &devices
        )

        guard status == noErr else { return nil }

        // Find AES67 device by name
        for device in devices {
            var namePropertyAddress = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceName,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )

            var nameSize: UInt32 = 0
            status = AudioObjectGetPropertyDataSize(device, &namePropertyAddress, 0, nil, &nameSize)
            guard status == noErr else { continue }

            var name = [CChar](repeating: 0, count: Int(nameSize))
            status = AudioObjectGetPropertyData(device, &namePropertyAddress, 0, nil, &nameSize, &name)
            guard status == noErr else { continue }

            let deviceName = String(cString: name)
            if deviceName.contains("AES67") {
                return device
            }
        }

        return nil
    }

    /// Returns a formatted string for the sample rate
    static func formatSampleRate(_ rate: Double) -> String {
        if rate >= 1000 {
            let khz = rate / 1000.0
            if khz.truncatingRemainder(dividingBy: 1) == 0 {
                return "\(Int(khz)) kHz"
            } else {
                return String(format: "%.1f kHz", khz)
            }
        }
        return "\(Int(rate)) Hz"
    }

    deinit {
        refreshTimer?.invalidate()
    }
}
