//
// ChannelMappingView.swift
// AES67 Manager - Build #17
// Full 128-channel interactive mapping visualizer with simple/advanced modes
//

import SwiftUI

// MARK: - View Mode

enum ChannelViewMode: String, CaseIterable {
    case simple    // Only show used channels + next available
    case advanced  // Show all 128 channels (current behavior)

    var label: String {
        switch self {
        case .simple: return "Simple"
        case .advanced: return "Advanced"
        }
    }
}

struct ChannelMappingView: View {
    @ObservedObject var driverManager: DriverManager
    @State private var selectedChannel: Int? = nil
    @State private var selectedStream: StreamInfo? = nil
    @State private var hoveredChannel: Int? = nil
    @State private var viewMode: ChannelViewMode = .simple

    private let totalChannels = 128
    private let channelsPerRow = 16
    private let rows = 8

    var body: some View {
        VStack(spacing: 0) {
            // Header with view mode toggle
            headerView

            Divider()

            // Content based on view mode
            Group {
                switch viewMode {
                case .simple:
                    simpleView
                case .advanced:
                    advancedGridView
                }
            }
            .frame(maxHeight: .infinity)
        }
        .background(Color(nsColor: .controlBackgroundColor))
    }

    // MARK: - Header View

    private var headerView: some View {
        HStack {
            VStack(alignment: .leading, spacing: 4) {
                Text("Channel Mapping")
                    .font(.title2)
                    .fontWeight(.bold)

                Text("\(usedChannelCount)/128 channels assigned")
                    .font(.subheadline)
                    .foregroundColor(.secondary)
            }

            Spacer()

            // View mode toggle
            Picker("View", selection: $viewMode) {
                ForEach(ChannelViewMode.allCases, id: \.self) { mode in
                    Text(mode.label).tag(mode)
                }
            }
            .pickerStyle(.segmented)
            .frame(width: 180)

            Spacer()
                .frame(width: 20)

            // Legend (only shown in advanced mode)
            if viewMode == .advanced {
                HStack(spacing: 16) {
                    LegendItem(color: .gray.opacity(0.2), label: "Available")
                    LegendItem(color: .blue, label: "Assigned")
                    LegendItem(color: .green, label: "Selected")
                }
            }
        }
        .padding()
    }

    // MARK: - Simple View

    @ViewBuilder
    private var simpleView: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                // Summary Card
                summaryCard

                // Stream Assignments Section
                streamAssignmentsSection

                // Quick Actions Section
                quickActionsSection
            }
            .padding()
        }
    }

    private var summaryCard: some View {
        HStack(spacing: 24) {
            // Channels Used
            VStack(alignment: .leading, spacing: 4) {
                Text("Channels Used")
                    .font(.caption)
                    .foregroundColor(.secondary)
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text("\(usedChannelCount)")
                        .font(.system(size: 32, weight: .bold, design: .rounded))
                        .foregroundColor(.blue)
                    Text("/ 128")
                        .font(.headline)
                        .foregroundColor(.secondary)
                }
            }

            Divider()
                .frame(height: 50)

            // Streams Mapped
            VStack(alignment: .leading, spacing: 4) {
                Text("Streams Mapped")
                    .font(.caption)
                    .foregroundColor(.secondary)
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text("\(mappedStreamCount)")
                        .font(.system(size: 32, weight: .bold, design: .rounded))
                        .foregroundColor(.green)
                    Text("/ \(driverManager.streams.count)")
                        .font(.headline)
                        .foregroundColor(.secondary)
                }
            }

            Divider()
                .frame(height: 50)

            // Available Channels
            VStack(alignment: .leading, spacing: 4) {
                Text("Available")
                    .font(.caption)
                    .foregroundColor(.secondary)
                HStack(alignment: .firstTextBaseline, spacing: 4) {
                    Text("\(availableChannelCount)")
                        .font(.system(size: 32, weight: .bold, design: .rounded))
                        .foregroundColor(availableChannelCount > 0 ? .green : .orange)
                    Text("channels")
                        .font(.headline)
                        .foregroundColor(.secondary)
                }
            }

            Spacer()

            // Visual Progress
            VStack(alignment: .trailing, spacing: 4) {
                Text("Utilization")
                    .font(.caption)
                    .foregroundColor(.secondary)
                ProgressView(value: Double(usedChannelCount), total: 128)
                    .progressViewStyle(.linear)
                    .frame(width: 120)
                Text("\(Int(Double(usedChannelCount) / 128.0 * 100))%")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .padding()
        .background(Color(nsColor: .textBackgroundColor))
        .cornerRadius(12)
    }

    private var streamAssignmentsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Stream Assignments")
                .font(.headline)

            if driverManager.streams.isEmpty {
                emptyStateView
            } else {
                ForEach(driverManager.streams) { stream in
                    SimpleStreamRow(
                        stream: stream,
                        color: colorForStream(stream),
                        onAutoMap: { autoMapStream(stream) },
                        onClearMapping: { clearMapping(for: stream) }
                    )
                }
            }
        }
    }

    private var emptyStateView: some View {
        VStack(spacing: 12) {
            Image(systemName: "waveform.path")
                .font(.system(size: 48))
                .foregroundColor(.secondary.opacity(0.5))

            Text("No Streams Added")
                .font(.headline)
                .foregroundColor(.secondary)

            Text("Add streams from the Stream List to begin mapping channels.")
                .font(.subheadline)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
        .padding(40)
        .background(Color(nsColor: .textBackgroundColor))
        .cornerRadius(12)
    }

    private var quickActionsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Quick Actions")
                .font(.headline)

            HStack(spacing: 12) {
                Button(action: autoMapAllStreams) {
                    HStack {
                        Image(systemName: "wand.and.stars")
                        Text("Auto-Map All Streams")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(unmappedStreamCount == 0)

                Button(action: clearAllMappings) {
                    HStack {
                        Image(systemName: "trash")
                        Text("Clear All Mappings")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .foregroundColor(.red)
                .disabled(mappedStreamCount == 0)

                Button(action: { viewMode = .advanced }) {
                    HStack {
                        Image(systemName: "square.grid.3x3")
                        Text("Advanced View")
                    }
                    .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }

            // Info text
            if unmappedStreamCount > 0 {
                HStack {
                    Image(systemName: "info.circle")
                        .foregroundColor(.blue)
                    Text("\(unmappedStreamCount) stream\(unmappedStreamCount == 1 ? "" : "s") not yet mapped. Click 'Auto-Map All' to assign channels automatically.")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .padding(.top, 4)
            }
        }
    }

    // MARK: - Advanced Grid View (Original Implementation)

    private var advancedGridView: some View {
        HStack(alignment: .top, spacing: 20) {
            // Channel Grid
            VStack(alignment: .leading, spacing: 12) {
                Text("128-Channel Device Layout")
                    .font(.headline)
                    .padding(.horizontal)

                channelGridView
                    .padding(.horizontal)

                channelInfoBar
                    .padding(.horizontal)
            }
            .frame(maxWidth: .infinity)

            Divider()

            // Stream List & Controls
            VStack(alignment: .leading, spacing: 12) {
                Text("Active Streams")
                    .font(.headline)

                streamListView

                Spacer()

                if let stream = selectedStream {
                    mappingControlsView(for: stream)
                }
            }
            .frame(width: 280)
            .padding()
        }
    }

    // MARK: - Channel Grid View

    private var channelGridView: some View {
        VStack(spacing: 2) {
            ForEach(0..<rows, id: \.self) { row in
                HStack(spacing: 2) {
                    // Row label
                    Text("\(row * channelsPerRow)")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(width: 30, alignment: .trailing)

                    // Channel cells
                    ForEach(0..<channelsPerRow, id: \.self) { col in
                        let channelNum = row * channelsPerRow + col
                        ChannelCell(
                            channelNumber: channelNum,
                            mapping: mappingForChannel(channelNum),
                            isSelected: selectedChannel == channelNum,
                            isHovered: hoveredChannel == channelNum,
                            isInSelectedStream: isChannelInSelectedStream(channelNum)
                        )
                        .onTapGesture {
                            handleChannelTap(channelNum)
                        }
                        .onHover { hovering in
                            hoveredChannel = hovering ? channelNum : nil
                        }
                    }
                }
            }

            // Column labels
            HStack(spacing: 2) {
                Spacer()
                    .frame(width: 30)

                ForEach(0..<channelsPerRow, id: \.self) { col in
                    Text("\(col)")
                        .font(.system(size: 9, design: .monospaced))
                        .foregroundColor(.secondary)
                        .frame(maxWidth: .infinity)
                }
            }
        }
        .padding(8)
        .background(Color(nsColor: .textBackgroundColor))
        .cornerRadius(8)
    }

    // MARK: - Channel Info Bar

    private var channelInfoBar: some View {
        HStack {
            if let channel = hoveredChannel ?? selectedChannel {
                Text("Channel \(channel)")
                    .font(.system(.body, design: .monospaced))
                    .fontWeight(.semibold)

                if let mapping = mappingForChannel(channel) {
                    Text("*")
                        .foregroundColor(.secondary)
                    Text(mapping.streamName)
                        .foregroundColor(.blue)
                    Text("(Stream Ch \(channel - Int(mapping.deviceChannelStart)))")
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else {
                    Text("*")
                        .foregroundColor(.secondary)
                    Text("Available")
                        .foregroundColor(.secondary)
                }
            } else {
                Text("Hover over a channel for details")
                    .foregroundColor(.secondary)
            }

            Spacer()
        }
        .padding(8)
        .background(Color(nsColor: .controlBackgroundColor))
        .cornerRadius(6)
    }

    // MARK: - Stream List View

    private var streamListView: some View {
        ScrollView {
            VStack(spacing: 8) {
                ForEach(driverManager.streams) { stream in
                    StreamMappingRow(
                        stream: stream,
                        isSelected: selectedStream?.id == stream.id,
                        color: colorForStream(stream)
                    )
                    .onTapGesture {
                        selectedStream = stream
                    }
                }
            }
        }
    }

    // MARK: - Mapping Controls

    private func mappingControlsView(for stream: StreamInfo) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Divider()

            Text("Map Stream")
                .font(.headline)

            VStack(alignment: .leading, spacing: 8) {
                Text(stream.name)
                    .font(.subheadline)
                    .fontWeight(.medium)

                Text("\(stream.numChannels) channels")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            if let mapping = stream.mapping {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Current Mapping:")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Text("Device Ch \(mapping.deviceChannelStart)-\(mapping.deviceChannelStart + mapping.deviceChannelCount - 1)")
                        .font(.system(.body, design: .monospaced))
                }
                .padding(8)
                .background(Color.blue.opacity(0.1))
                .cornerRadius(6)

                Button("Clear Mapping") {
                    clearMapping(for: stream)
                }
                .buttonStyle(.bordered)
            } else {
                Button("Auto-Assign Channels") {
                    autoAssignChannels(for: stream)
                }
                .buttonStyle(.borderedProminent)
            }
        }
        .padding(12)
        .background(Color(nsColor: .controlBackgroundColor))
        .cornerRadius(8)
    }

    // MARK: - Computed Properties

    private var usedChannelCount: Int {
        var count = 0
        for stream in driverManager.streams {
            if let mapping = stream.mapping {
                count += Int(mapping.deviceChannelCount)
            }
        }
        return count
    }

    private var availableChannelCount: Int {
        totalChannels - usedChannelCount
    }

    private var mappedStreamCount: Int {
        driverManager.streams.filter { $0.mapping != nil }.count
    }

    private var unmappedStreamCount: Int {
        driverManager.streams.filter { $0.mapping == nil }.count
    }

    // MARK: - Helper Functions

    private func mappingForChannel(_ channel: Int) -> ChannelMappingInfo? {
        for stream in driverManager.streams {
            if let mapping = stream.mapping {
                let start = Int(mapping.deviceChannelStart)
                let end = start + Int(mapping.deviceChannelCount)
                if channel >= start && channel < end {
                    return mapping
                }
            }
        }
        return nil
    }

    private func isChannelInSelectedStream(_ channel: Int) -> Bool {
        guard let stream = selectedStream,
              let mapping = stream.mapping else {
            return false
        }
        let start = Int(mapping.deviceChannelStart)
        let end = start + Int(mapping.deviceChannelCount)
        return channel >= start && channel < end
    }

    private func colorForStream(_ stream: StreamInfo) -> Color {
        // Generate consistent color based on stream ID
        let hash = stream.id.hashValue
        let hue = Double(abs(hash) % 360) / 360.0
        return Color(hue: hue, saturation: 0.6, brightness: 0.8)
    }

    private func handleChannelTap(_ channel: Int) {
        selectedChannel = channel
    }

    private func autoAssignChannels(for stream: StreamInfo) {
        // Find first available contiguous block
        let neededChannels = Int(stream.numChannels)
        guard neededChannels > 0 && neededChannels <= totalChannels else { return }

        for startCh in 0...(totalChannels - neededChannels) {
            var available = true
            for ch in startCh..<(startCh + neededChannels) {
                if mappingForChannel(ch) != nil {
                    available = false
                    break
                }
            }

            if available {
                // Found a spot - create mapping
                driverManager.assignMapping(
                    streamID: stream.id,
                    deviceChannelStart: UInt16(startCh),
                    deviceChannelCount: stream.numChannels
                )
                break
            }
        }
    }

    private func autoMapStream(_ stream: StreamInfo) {
        // Same as autoAssignChannels but called from simple view
        autoAssignChannels(for: stream)
    }

    private func clearMapping(for stream: StreamInfo) {
        driverManager.clearMapping(streamID: stream.id)
    }

    private func autoMapAllStreams() {
        // Auto-map all unmapped streams in order
        for stream in driverManager.streams {
            if stream.mapping == nil {
                autoAssignChannels(for: stream)
            }
        }
    }

    private func clearAllMappings() {
        for stream in driverManager.streams {
            if stream.mapping != nil {
                driverManager.clearMapping(streamID: stream.id)
            }
        }
    }
}

// MARK: - Simple Stream Row

struct SimpleStreamRow: View {
    let stream: StreamInfo
    let color: Color
    let onAutoMap: () -> Void
    let onClearMapping: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            // Color indicator
            Circle()
                .fill(color)
                .frame(width: 12, height: 12)

            // Stream info
            VStack(alignment: .leading, spacing: 4) {
                Text(stream.name)
                    .font(.subheadline)
                    .fontWeight(.medium)
                    .lineLimit(1)

                HStack(spacing: 8) {
                    // Channel count
                    Label("\(stream.numChannels) ch", systemImage: "waveform")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    // Mapping status
                    if let mapping = stream.mapping {
                        Label("Ch \(mapping.deviceChannelStart + 1)-\(mapping.deviceChannelStart + mapping.deviceChannelCount)",
                              systemImage: "checkmark.circle.fill")
                            .font(.caption)
                            .foregroundColor(.green)
                    } else {
                        Label("Not mapped", systemImage: "exclamationmark.circle")
                            .font(.caption)
                            .foregroundColor(.orange)
                    }
                }
            }

            Spacer()

            // Actions
            if stream.mapping != nil {
                Button(action: onClearMapping) {
                    Image(systemName: "xmark.circle")
                        .foregroundColor(.secondary)
                }
                .buttonStyle(.plain)
                .help("Clear mapping")
            } else {
                Button("Auto-Map", action: onAutoMap)
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            }
        }
        .padding(12)
        .background(Color(nsColor: .textBackgroundColor))
        .cornerRadius(8)
        .overlay(
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(stream.mapping != nil ? Color.green.opacity(0.3) : Color.orange.opacity(0.3), lineWidth: 1)
        )
    }
}

// MARK: - Channel Cell

struct ChannelCell: View {
    let channelNumber: Int
    let mapping: ChannelMappingInfo?
    let isSelected: Bool
    let isHovered: Bool
    let isInSelectedStream: Bool

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 3)
                .fill(backgroundColor)
                .overlay(
                    RoundedRectangle(cornerRadius: 3)
                        .strokeBorder(borderColor, lineWidth: borderWidth)
                )

            if isHovered || isSelected {
                Text("\(channelNumber)")
                    .font(.system(size: 8, design: .monospaced))
                    .foregroundColor(.white)
            }
        }
        .frame(height: 28)
        .help("Channel \(channelNumber)\(mapping != nil ? " - \(mapping!.streamName)" : "")")
    }

    private var backgroundColor: Color {
        if isSelected {
            return .green.opacity(0.8)
        } else if isInSelectedStream {
            return .green.opacity(0.4)
        } else if mapping != nil {
            return .blue.opacity(0.6)
        } else if isHovered {
            return .gray.opacity(0.3)
        } else {
            return .gray.opacity(0.15)
        }
    }

    private var borderColor: Color {
        if isSelected || isHovered {
            return .white.opacity(0.5)
        } else {
            return .clear
        }
    }

    private var borderWidth: CGFloat {
        isSelected ? 2 : (isHovered ? 1 : 0)
    }
}

// MARK: - Stream Mapping Row

struct StreamMappingRow: View {
    let stream: StreamInfo
    let isSelected: Bool
    let color: Color

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(color)
                .frame(width: 12, height: 12)

            VStack(alignment: .leading, spacing: 2) {
                Text(stream.name)
                    .font(.subheadline)
                    .fontWeight(isSelected ? .semibold : .regular)
                    .lineLimit(1)

                if let mapping = stream.mapping {
                    Text("Ch \(mapping.deviceChannelStart)-\(mapping.deviceChannelStart + mapping.deviceChannelCount - 1)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                } else {
                    Text("Not mapped")
                        .font(.caption)
                        .foregroundColor(.orange)
                }
            }

            Spacer()

            Text("\(stream.numChannels)")
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .padding(8)
        .background(isSelected ? Color.blue.opacity(0.15) : Color.clear)
        .cornerRadius(6)
        .overlay(
            RoundedRectangle(cornerRadius: 6)
                .strokeBorder(isSelected ? Color.blue : Color.clear, lineWidth: 1)
        )
    }
}

// MARK: - Legend Item

struct LegendItem: View {
    let color: Color
    let label: String

    var body: some View {
        HStack(spacing: 4) {
            RoundedRectangle(cornerRadius: 2)
                .fill(color)
                .frame(width: 12, height: 12)
            Text(label)
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }
}

// MARK: - Preview

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview {
    ChannelMappingView(driverManager: DriverManager())
        .frame(width: 1200, height: 800)
}
#endif
