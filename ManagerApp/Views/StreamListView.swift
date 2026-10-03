//
// StreamListView.swift
// AES67 Manager - Build #15
// Sidebar list of active streams with SDP drag-and-drop support and audio level indicators
//

import SwiftUI
import UniformTypeIdentifiers
import AppKit

// MARK: - SDP Parsing Error

enum SDPParseError: LocalizedError {
    case emptyContent
    case missingMulticastAddress
    case invalidMulticastAddress(String)
    case invalidPort(String)
    case invalidSampleRate(String)
    case invalidChannelCount(String)
    case noAudioMediaSection

    var errorDescription: String? {
        switch self {
        case .emptyContent:
            return "SDP content is empty"
        case .missingMulticastAddress:
            return "Missing multicast address (c= line)"
        case .invalidMulticastAddress(let addr):
            return "Invalid multicast address: \(addr)"
        case .invalidPort(let port):
            return "Invalid port number: \(port)"
        case .invalidSampleRate(let rate):
            return "Invalid sample rate: \(rate)"
        case .invalidChannelCount(let count):
            return "Invalid channel count: \(count)"
        case .noAudioMediaSection:
            return "No audio media section found (m=audio line)"
        }
    }
}

// MARK: - Parsed SDP Result

struct ParsedSDPInfo {
    var name: String = "Imported Stream"
    var description: String?
    var multicastIP: String = ""
    var port: UInt16 = 5004
    var numChannels: UInt16 = 2
    var sampleRate: UInt32 = 48000
    var encoding: String = "L24"
    var payloadType: UInt8 = 97
    var ttl: UInt8 = 32
    var sourceIP: String?
    var ptpDomain: Int = 0
}

struct StreamListView: View {
    @EnvironmentObject var driverManager: DriverManager
    @Binding var selectedStream: StreamInfo?

    @State private var showingErrorAlert = false
    @State private var errorMessage = ""
    @State private var showingSuccessAlert = false
    @State private var successMessage = ""

    // Confirmation dialog for removal
    @State private var streamToRemove: StreamInfo?
    @State private var showingRemoveConfirmation = false

    // Sample rate mismatch handling for SDP imports
    @State private var showingSampleRateMismatchAlert = false
    @State private var pendingSDPImport: ParsedSDPInfo?

    var body: some View {
        ZStack {
        List(selection: $selectedStream) {
            Section("Active Streams (\(driverManager.streams.count))") {
                ForEach(driverManager.streams) { stream in
                    StreamRowView(stream: stream)
                        .tag(stream)
                        .contextMenu {
                            Button("Remove Stream", role: .destructive) {
                                streamToRemove = stream
                                showingRemoveConfirmation = true
                            }

                            Button("Export SDP...") {
                                driverManager.exportSDP(for: stream)
                            }

                            Divider()

                            Button("View Details") {
                                selectedStream = stream
                            }
                        }
                }
            }

            Section("Statistics") {
                HStack {
                    Text("Total Channels")
                    Spacer()
                    Text("\(driverManager.totalChannelsUsed)/128")
                        .foregroundColor(.secondary)
                }

                HStack {
                    Text("Available")
                    Spacer()
                    Text("\(128 - driverManager.totalChannelsUsed)")
                        .foregroundColor(.secondary)
                }
            }
        }
        .listStyle(.sidebar)
        .onDrop(of: [.plainText, .fileURL], delegate: StreamListSDPDropDelegate(
            onDrop: { providers in
                _ = handleSDPDrops(providers: providers)
                return true
            }
        ))
        .toolbar {
            ToolbarItem {
                PTPDiagnosticButton()
            }
        }
        .alert("Import Error", isPresented: $showingErrorAlert) {
            Button("OK", role: .cancel) { }
        } message: {
            Text(errorMessage)
        }
        .alert("Stream Imported", isPresented: $showingSuccessAlert) {
            Button("OK", role: .cancel) { }
        } message: {
            Text(successMessage)
        }
        .confirmationDialog(
            "Remove Stream",
            isPresented: $showingRemoveConfirmation,
            presenting: streamToRemove
        ) { stream in
            Button("Remove \"\(stream.name)\"", role: .destructive) {
                driverManager.removeStream(stream)
                if selectedStream?.id == stream.id {
                    selectedStream = nil
                }
            }
            Button("Cancel", role: .cancel) { }
        } message: { stream in
            Text("Are you sure you want to remove \"\(stream.name)\"? This will stop receiving audio from this stream.")
        }

            // Sample rate mismatch overlay for SDP imports
            if showingSampleRateMismatchAlert, let pending = pendingSDPImport {
                Color.black.opacity(0.3)
                    .ignoresSafeArea()

                SampleRateMismatchAlert(
                    streamRate: pending.sampleRate,
                    deviceRate: driverManager.currentDeviceSampleRate,
                    onChangeDevice: {
                        handleChangeSampleRateForSDPImport()
                    },
                    onCancel: {
                        showingSampleRateMismatchAlert = false
                        pendingSDPImport = nil
                    }
                )
                .shadow(radius: 20)
                .transition(.scale.combined(with: .opacity))
            }
        }
        .animation(.easeInOut(duration: 0.2), value: showingSampleRateMismatchAlert)
    }

    // MARK: - Sample Rate Mismatch Handling for SDP Import

    private func handleChangeSampleRateForSDPImport() {
        guard let pending = pendingSDPImport else { return }

        // Change device sample rate
        if driverManager.setDeviceSampleRate(Double(pending.sampleRate)) {
            // Add the stream with bypassed check
            _ = driverManager.addStream(
                name: pending.name,
                multicastIP: pending.multicastIP,
                port: pending.port,
                numChannels: pending.numChannels,
                sampleRate: pending.sampleRate,
                encoding: pending.encoding,
                bypassSampleRateCheck: true
            )

            successMessage = "Added stream '\(pending.name)' (\(pending.numChannels) channels, \(formatSampleRate(pending.sampleRate))). Device sample rate changed to \(formatSampleRate(pending.sampleRate))."
            showingSuccessAlert = true
        }

        showingSampleRateMismatchAlert = false
        pendingSDPImport = nil
    }

    private func handleSDPDrops(providers: [NSItemProvider]) -> Bool {
        for provider in providers {
            if provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    guard let fileURL = url else { return }

                    // Verify it's an SDP file
                    if fileURL.pathExtension.lowercased() == "sdp" {
                        Task {
                            await self.parseAndAddSDPFile(url: fileURL)
                        }
                    }
                }
            } else if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
                _ = provider.loadObject(ofClass: NSString.self) { string, _ in
                    guard let sdpContent = string as? String else { return }

                    Task {
                        await self.parseAndAddSDPContent(content: sdpContent)
                    }
                }
            }
        }

        return true
    }

    private func parseAndAddSDPFile(url: URL) async {
        do {
            // Read file content
            let sdpContent = try String(contentsOf: url, encoding: .utf8)

            // Use filename as fallback stream name
            let fileName = url.deletingPathExtension().lastPathComponent
            await parseAndAddSDPContent(content: sdpContent, fallbackName: fileName)
        } catch {
            await showError("Error reading SDP file: \(error.localizedDescription)")
        }
    }

    private func parseAndAddSDPContent(content: String, fallbackName: String? = nil) async {
        do {
            let parsed = try parseSDPContent(content, fallbackName: fallbackName)

            // Add stream via DriverManager on main thread
            await MainActor.run {
                // Check for sample rate mismatch before adding
                if parsed.sampleRate != UInt32(driverManager.currentDeviceSampleRate) {
                    // Store pending import and show mismatch alert
                    pendingSDPImport = parsed
                    showingSampleRateMismatchAlert = true
                    return
                }

                // No mismatch, add directly
                let result = driverManager.addStream(
                    name: parsed.name,
                    multicastIP: parsed.multicastIP,
                    port: parsed.port,
                    numChannels: parsed.numChannels,
                    sampleRate: parsed.sampleRate,
                    encoding: parsed.encoding,
                    bypassSampleRateCheck: true
                )

                switch result {
                case .success:
                    // Show success notification
                    successMessage = "Added stream '\(parsed.name)' (\(parsed.numChannels) channels, \(formatSampleRate(parsed.sampleRate)))"
                    showingSuccessAlert = true
                case .sampleRateMismatch:
                    // This shouldn't happen with bypass, but handle just in case
                    pendingSDPImport = parsed
                    showingSampleRateMismatchAlert = true
                case .channelLimitExceeded:
                    errorMessage = "Cannot add stream: would exceed 128 channel limit"
                    showingErrorAlert = true
                }
            }

        } catch let error as SDPParseError {
            await showError(error.localizedDescription)
        } catch {
            await showError("Failed to parse SDP: \(error.localizedDescription)")
        }
    }

    // MARK: - SDP Parser

    private func parseSDPContent(_ content: String, fallbackName: String? = nil) throws -> ParsedSDPInfo {
        let trimmedContent = content.trimmingCharacters(in: .whitespacesAndNewlines)

        guard !trimmedContent.isEmpty else {
            throw SDPParseError.emptyContent
        }

        var parsed = ParsedSDPInfo()

        // Use fallback name if provided
        if let fallbackName = fallbackName, !fallbackName.isEmpty {
            parsed.name = fallbackName
        }

        let lines = content.components(separatedBy: .newlines)
        var foundAudioMedia = false
        var inAudioSection = false

        for line in lines {
            let trimmed = line.trimmingCharacters(in: .whitespaces)

            // Skip empty lines and comments
            guard !trimmed.isEmpty else { continue }

            // Session name (s=)
            if trimmed.hasPrefix("s=") {
                let sessionName = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                if !sessionName.isEmpty && sessionName != "-" {
                    parsed.name = sessionName
                }
            }

            // Session information/description (i=)
            else if trimmed.hasPrefix("i=") {
                let info = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                if !info.isEmpty {
                    parsed.description = info
                }
            }

            // Connection data (c=)
            // Format: c=IN IP4 <address>/<ttl> or c=IN IP4 <address>
            else if trimmed.hasPrefix("c=") {
                let connectionData = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                let parts = connectionData.components(separatedBy: .whitespaces).filter { !$0.isEmpty }

                // Expected format: IN IP4 <address>[/<ttl>]
                if parts.count >= 3 {
                    let addressPart = parts[2]
                    let addressComponents = addressPart.components(separatedBy: "/")

                    parsed.multicastIP = addressComponents[0]

                    // Parse TTL if present
                    if addressComponents.count >= 2, let ttl = UInt8(addressComponents[1]) {
                        parsed.ttl = ttl
                    }
                }
            }

            // Origin (o=) - can contain source IP
            // Format: o=<username> <session-id> <session-version> IN IP4 <address>
            else if trimmed.hasPrefix("o=") {
                let originData = String(trimmed.dropFirst(2)).trimmingCharacters(in: .whitespaces)
                let parts = originData.components(separatedBy: .whitespaces).filter { !$0.isEmpty }

                // Find IP4 followed by address
                if let ip4Index = parts.firstIndex(of: "IP4"), ip4Index + 1 < parts.count {
                    let sourceAddr = parts[ip4Index + 1]
                    // Only set as source if it's not a placeholder
                    if sourceAddr != "127.0.0.1" && sourceAddr != "0.0.0.0" {
                        parsed.sourceIP = sourceAddr
                    }
                }
            }

            // Media description (m=)
            // Format: m=audio <port> RTP/AVP <payload-type>
            else if trimmed.hasPrefix("m=audio ") {
                foundAudioMedia = true
                inAudioSection = true

                let mediaData = String(trimmed.dropFirst(8)).trimmingCharacters(in: .whitespaces)
                let parts = mediaData.components(separatedBy: .whitespaces).filter { !$0.isEmpty }

                // Parse port
                if !parts.isEmpty {
                    // Port can be just a number or port/count
                    let portStr = parts[0].components(separatedBy: "/").first ?? parts[0]
                    if let port = UInt16(portStr), port > 0 {
                        parsed.port = port
                    } else {
                        throw SDPParseError.invalidPort(parts[0])
                    }
                }

                // Parse payload type (last element)
                if parts.count >= 3, let pt = UInt8(parts.last ?? "") {
                    parsed.payloadType = pt
                }
            }

            // Other media types end the audio section
            else if trimmed.hasPrefix("m=") && !trimmed.hasPrefix("m=audio") {
                inAudioSection = false
            }

            // Attribute lines (a=) - only process in audio section
            else if trimmed.hasPrefix("a=") && (inAudioSection || !foundAudioMedia) {
                let attrData = String(trimmed.dropFirst(2))

                // rtpmap - audio codec information
                // Format: a=rtpmap:<payload-type> <encoding>/<clock-rate>[/<channels>]
                if attrData.hasPrefix("rtpmap:") {
                    // Try to extract encoding/sampleRate/channels pattern
                    // Matches patterns like: L24/48000/2, L16/44100/8, etc.
                    if let match = attrData.range(of: #"(L\d+|AM824)/(\d+)(?:/(\d+))?"#, options: .regularExpression) {
                        let rtpInfo = String(attrData[match])
                        let rtpParts = rtpInfo.components(separatedBy: "/")

                        if rtpParts.count >= 1 {
                            parsed.encoding = rtpParts[0]
                        }

                        if rtpParts.count >= 2 {
                            if let rate = UInt32(rtpParts[1]) {
                                parsed.sampleRate = rate
                            }
                        }

                        if rtpParts.count >= 3 {
                            if let channels = UInt16(rtpParts[2]) {
                                parsed.numChannels = channels
                            }
                        }
                    }
                }

                // fmtp - format parameters (may contain channel count)
                // Format: a=fmtp:<payload-type> <parameters>
                else if attrData.hasPrefix("fmtp:") {
                    // Look for channel-order parameter which indicates channel count
                    if let channelMatch = attrData.range(of: #"channel-order\s*=\s*SMPTE2110\.\((\d+)\)"#, options: .regularExpression) {
                        let channelStr = String(attrData[channelMatch])
                        if let numMatch = channelStr.range(of: #"\d+"#, options: .regularExpression) {
                            if let channels = UInt16(channelStr[numMatch]) {
                                parsed.numChannels = channels
                            }
                        }
                    }
                }

                // ptp-domain attribute for AES67
                else if attrData.hasPrefix("clock-domain:") || attrData.contains("ptp-domain") {
                    if let domainMatch = attrData.range(of: #"\d+"#, options: .regularExpression) {
                        if let domain = Int(attrData[domainMatch]) {
                            parsed.ptpDomain = domain
                        }
                    }
                }

                // source-filter for SSM (Source-Specific Multicast)
                // Format: a=source-filter: incl IN IP4 <dest-addr> <src-addr>
                else if attrData.hasPrefix("source-filter:") {
                    let filterParts = attrData.components(separatedBy: .whitespaces).filter { !$0.isEmpty }
                    // Last element should be the source IP
                    if filterParts.count >= 5 {
                        parsed.sourceIP = filterParts.last
                    }
                }
            }
        }

        // Validate required fields
        guard foundAudioMedia else {
            throw SDPParseError.noAudioMediaSection
        }

        guard !parsed.multicastIP.isEmpty else {
            throw SDPParseError.missingMulticastAddress
        }

        // Validate multicast address format (basic check)
        if !isValidMulticastAddress(parsed.multicastIP) {
            throw SDPParseError.invalidMulticastAddress(parsed.multicastIP)
        }

        // Validate sample rate is reasonable
        let validSampleRates: [UInt32] = [44100, 48000, 88200, 96000, 176400, 192000]
        if !validSampleRates.contains(parsed.sampleRate) {
            // Allow it but log a warning - some devices use non-standard rates
            print("Warning: Unusual sample rate \(parsed.sampleRate) Hz")
        }

        return parsed
    }

    // MARK: - Validation Helpers

    private func isValidMulticastAddress(_ address: String) -> Bool {
        // IPv4 multicast range: 224.0.0.0 to 239.255.255.255
        let parts = address.components(separatedBy: ".")
        guard parts.count == 4 else { return false }

        guard let firstOctet = Int(parts[0]) else { return false }

        // Check if it's in multicast range (224-239) or allow unicast for testing
        return (firstOctet >= 224 && firstOctet <= 239) || (firstOctet >= 1 && firstOctet <= 223)
    }

    // MARK: - UI Helpers

    private func showError(_ message: String) async {
        await MainActor.run {
            errorMessage = message
            showingErrorAlert = true
        }
    }

    private func formatSampleRate(_ rate: UInt32) -> String {
        if rate >= 1000 {
            return "\(rate / 1000)kHz"
        }
        return "\(rate)Hz"
    }
}

// Define the drop delegate to handle SDP file drops (local to StreamListView)
fileprivate struct StreamListSDPDropDelegate: DropDelegate {
    let onDrop: ([NSItemProvider]) -> Bool

    func performDrop(info: DropInfo) -> Bool {
        let providers: [NSItemProvider] = info.itemProviders(for: [.plainText, .fileURL])
        return onDrop(providers)
    }
}

struct PTPDiagnosticButton: View {
    @EnvironmentObject var driverManager: DriverManager
    @State private var showingDiagnostic = false

    var body: some View {
        Button("PTP Diagnostics") {
            showingDiagnostic = true
        }
        .sheet(isPresented: $showingDiagnostic) {
            PTPDiagnosticView()
                .environmentObject(driverManager)
        }
    }
}

struct StreamRowView: View {
    let stream: StreamInfo
    @State private var hasSignal = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Image(systemName: stream.isConnected ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash")
                    .foregroundColor(stream.isConnected ? .green : .orange)
                    .font(.caption)

                Text(stream.name)
                    .font(.headline)

                Spacer()

                // Signal present indicator (green dot)
                if stream.isConnected {
                    Circle()
                        .fill(hasSignal ? Color.green : Color.gray.opacity(0.3))
                        .frame(width: 6, height: 6)
                        .shadow(color: hasSignal ? Color.green.opacity(0.5) : .clear, radius: 2)
                        .help(hasSignal ? "Audio signal detected" : "No audio signal")
                }
            }

            HStack(spacing: 4) {
                Text("\(stream.numChannels)ch")
                Text("\u{00B7}")
                Text("\(formatSampleRate(stream.sampleRate))")
                Text("\u{00B7}")
                Text(stream.encoding)

                Spacer()

                // Mini level indicator for first 2 channels
                if stream.isConnected && hasSignal {
                    MiniLevelIndicator()
                }
            }
            .font(.caption)
            .foregroundColor(.secondary)

            if let mapping = stream.mapping {
                Text("Channels \(mapping.deviceChannelStart)-\(mapping.deviceChannelStart + mapping.deviceChannelCount - 1)")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }
        }
        .padding(.vertical, 4)
        .onAppear {
            startSignalUpdates()
        }
    }

    private func formatSampleRate(_ rate: UInt32) -> String {
        if rate >= 1000 {
            return "\(rate / 1000)kHz"
        }
        return "\(rate)Hz"
    }

    private func startSignalUpdates() {
        // Show signal based on connection state
        // Real implementation would query driver for actual audio levels
        hasSignal = stream.isConnected
    }

}

/// Mini level indicator for sidebar stream rows
/// Shows a static green bar when connected (real data would come from driver)
struct MiniLevelIndicator: View {
    var body: some View {
        HStack(spacing: 1) {
            ForEach(0..<2, id: \.self) { _ in
                RoundedRectangle(cornerRadius: 1)
                    .fill(Color.green.opacity(0.6))
                    .frame(width: 12, height: 4)
            }
        }
    }
}

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview {
    StreamListView(selectedStream: .constant(nil))
        .environmentObject(DriverManager())
}
#endif
