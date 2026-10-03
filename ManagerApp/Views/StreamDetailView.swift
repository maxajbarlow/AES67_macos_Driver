//
// StreamDetailView.swift
// AES67 Manager - Build #15
// Detailed view of a stream's information with audio level meters
//

import SwiftUI

struct StreamDetailView: View {
    let stream: StreamInfo
    @State private var showExpandedMeters = false
    @AppStorage("showDetailedStats") private var showDetailedStats = true
    @AppStorage("showChannelViz") private var showChannelViz = true

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                // Header with signal indicator
                HStack {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack(spacing: 12) {
                            Text(stream.name)
                                .font(.largeTitle)
                                .fontWeight(.bold)

                            // Compact signal indicator in header
                            StreamSignalIndicator(stream: stream)
                        }

                        if let description = stream.description, !description.isEmpty {
                            Text(description)
                                .font(.subheadline)
                                .foregroundColor(.secondary)
                        }
                    }

                    Spacer()

                    StatusBadge(isConnected: stream.isConnected)
                }

                Divider()

                // Audio Levels Section - NEW
                StreamAudioStatusPanel(stream: stream)

                // Network Information
                GroupBox("Network") {
                    InfoRow(label: "Multicast IP", value: stream.multicastIP)
                    InfoRow(label: "Port", value: "\(stream.port)")
                    if let sourceIP = stream.sourceIP {
                        InfoRow(label: "Source IP", value: sourceIP)
                    }
                    InfoRow(label: "TTL", value: "\(stream.ttl)")
                }

                // Audio Format
                GroupBox("Audio Format") {
                    InfoRow(label: "Encoding", value: stream.encoding)
                    InfoRow(label: "Sample Rate", value: "\(stream.sampleRate) Hz")
                    InfoRow(label: "Channels", value: "\(stream.numChannels)")
                    InfoRow(label: "Payload Type", value: "\(stream.payloadType)")
                    if let bitrate = stream.bitrate {
                        InfoRow(label: "Bitrate", value: formatBitrate(bitrate))
                    }
                }

                // Channel Mapping
                if let mapping = stream.mapping {
                    GroupBox("Channel Mapping") {
                        InfoRow(label: "Device Channels",
                               value: "\(mapping.deviceChannelStart) - \(mapping.deviceChannelStart + mapping.deviceChannelCount - 1)")
                        InfoRow(label: "Stream Channels", value: "0 - \(mapping.streamChannelCount - 1)")

                        // Visual channel map (controlled by Settings toggle)
                        if showChannelViz {
                            ChannelMapVisualization(mapping: mapping)
                                .padding(.top, 8)
                        }
                    }
                }

                // Statistics (controlled by Settings toggle)
                if showDetailedStats, let stats = stream.statistics {
                    GroupBox("Statistics") {
                        InfoRow(label: "Packets Received", value: formatNumber(stats.packetsReceived))
                        InfoRow(label: "Packets Lost", value: formatNumber(stats.packetsLost))
                        InfoRow(label: "Loss Rate", value: String(format: "%.2f%%", stats.lossPercentage))
                        InfoRow(label: "Bytes Received", value: formatBytes(stats.bytesReceived))

                        if stats.underruns > 0 {
                            InfoRow(label: "Underruns", value: "\(stats.underruns)")
                                .foregroundColor(.orange)
                        }
                        if stats.overruns > 0 {
                            InfoRow(label: "Overruns", value: "\(stats.overruns)")
                                .foregroundColor(.orange)
                        }
                    }
                }

                // PTP Sync
                if stream.ptpDomain >= 0 {
                    GroupBox("PTP Synchronization") {
                        InfoRow(label: "Domain", value: "\(stream.ptpDomain)")
                        InfoRow(label: "Status", value: "Enabled")
                            .foregroundColor(.green)
                    }
                }

                Spacer()
            }
            .padding(24)
        }
        .background(Color(nsColor: .controlBackgroundColor))
    }

    private func formatBitrate(_ bitrate: UInt64) -> String {
        if bitrate >= 1_000_000 {
            return String(format: "%.1f Mbps", Double(bitrate) / 1_000_000.0)
        } else {
            return String(format: "%.0f kbps", Double(bitrate) / 1_000.0)
        }
    }

    private func formatNumber(_ num: UInt64) -> String {
        let formatter = NumberFormatter()
        formatter.numberStyle = .decimal
        return formatter.string(from: NSNumber(value: num)) ?? "\(num)"
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .binary
        return formatter.string(fromByteCount: Int64(bytes))
    }
}

struct StatusBadge: View {
    let isConnected: Bool

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(isConnected ? Color.green : Color.orange)
                .frame(width: 10, height: 10)
            Text(isConnected ? "Connected" : "Waiting")
                .font(.callout)
                .fontWeight(.medium)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(
            RoundedRectangle(cornerRadius: 12)
                .fill(isConnected ? Color.green.opacity(0.1) : Color.orange.opacity(0.1))
        )
    }
}

// MARK: - Stream Signal Indicator

/// Compact signal indicator for stream headers showing signal presence
/// Displays stable state based on connection status.
/// Real driver integration would query actual audio levels.
struct StreamSignalIndicator: View {
    let stream: StreamInfo

    private var hasSignal: Bool {
        stream.isConnected
    }

    var body: some View {
        HStack(spacing: 4) {
            Circle()
                .fill(hasSignal ? Color.green : Color.gray.opacity(0.4))
                .frame(width: 8, height: 8)
                .shadow(color: hasSignal ? Color.green.opacity(0.6) : .clear, radius: 3)

            Text(hasSignal ? "Signal" : "No Signal")
                .font(.caption2)
                .foregroundColor(hasSignal ? .green : .secondary)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(hasSignal ? Color.green.opacity(0.1) : Color.gray.opacity(0.1))
        )
    }
}

struct InfoRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack {
            Text(label)
                .foregroundColor(.secondary)
            Spacer()
            Text(value)
                .fontWeight(.medium)
        }
        .padding(.vertical, 2)
    }
}

struct ChannelMapVisualization: View {
    let mapping: ChannelMappingInfo

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Device Channel Allocation")
                .font(.caption)
                .foregroundColor(.secondary)

            HStack(spacing: 2) {
                // Show 16 blocks representing channels 0-15, 16-31, etc.
                ForEach(0..<8) { block in
                    let blockStart = block * 16
                    let blockEnd = blockStart + 16
                    let isUsed = blockStart < (mapping.deviceChannelStart + mapping.deviceChannelCount) &&
                                 blockEnd > mapping.deviceChannelStart

                    RoundedRectangle(cornerRadius: 2)
                        .fill(isUsed ? Color.blue : Color.gray.opacity(0.2))
                        .frame(height: 20)
                        .overlay(
                            Text("\(blockStart)")
                                .font(.system(size: 8))
                                .foregroundColor(isUsed ? .white : .secondary)
                        )
                }
            }
        }
    }
}

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview {
    StreamDetailView(stream: StreamInfo.example)
}
#endif
