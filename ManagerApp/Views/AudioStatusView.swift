//
// AudioStatusView.swift
// AES67 Manager - Build #15
// Audio level meters and status indicators for AES67 streams
//

import SwiftUI

// MARK: - Level Meter Component

/// A single horizontal level meter with gradient coloring and peak indicator
struct LevelMeter: View {
    let level: Float  // 0.0 to 1.0
    let peak: Float   // Peak hold level
    var showPeak: Bool = true
    var height: CGFloat = 8

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                // Background
                Rectangle()
                    .fill(Color.gray.opacity(0.3))

                // Level bar with gradient
                Rectangle()
                    .fill(levelGradient)
                    .frame(width: max(0, geo.size.width * CGFloat(level)))

                // Peak indicator
                if showPeak && peak > 0 {
                    Rectangle()
                        .fill(peakColor)
                        .frame(width: 2)
                        .offset(x: max(0, geo.size.width * CGFloat(peak) - 1))
                }

                // Clip indicator (red bar at the end when clipping)
                if level >= 0.98 {
                    Rectangle()
                        .fill(Color.red)
                        .frame(width: 3)
                        .offset(x: geo.size.width - 3)
                }
            }
        }
        .frame(height: height)
        .clipShape(RoundedRectangle(cornerRadius: 2))
    }

    private var levelGradient: LinearGradient {
        LinearGradient(
            colors: [.green, .green, .yellow, .orange, .red],
            startPoint: .leading,
            endPoint: .trailing
        )
    }

    private var peakColor: Color {
        if peak >= 0.95 {
            return .red
        } else if peak >= 0.75 {
            return .orange
        } else {
            return .white
        }
    }
}

// MARK: - Vertical Level Meter (for compact displays)

/// A vertical level meter for compact channel displays
struct VerticalLevelMeter: View {
    let level: Float
    let peak: Float
    var width: CGFloat = 12
    var showLabel: Bool = true
    var channelNumber: Int = 1

    var body: some View {
        VStack(spacing: 2) {
            GeometryReader { geo in
                ZStack(alignment: .bottom) {
                    // Background
                    Rectangle()
                        .fill(Color.gray.opacity(0.3))

                    // Level bar
                    Rectangle()
                        .fill(verticalGradient)
                        .frame(height: max(0, geo.size.height * CGFloat(level)))

                    // Peak indicator
                    if peak > 0 {
                        Rectangle()
                            .fill(Color.white)
                            .frame(height: 2)
                            .offset(y: -geo.size.height * CGFloat(peak) + geo.size.height)
                    }
                }
            }
            .frame(width: width)
            .clipShape(RoundedRectangle(cornerRadius: 2))

            if showLabel {
                Text("\(channelNumber)")
                    .font(.system(size: 8, weight: .medium))
                    .foregroundColor(.secondary)
            }
        }
    }

    private var verticalGradient: LinearGradient {
        LinearGradient(
            colors: [.green, .green, .yellow, .orange, .red],
            startPoint: .bottom,
            endPoint: .top
        )
    }
}

// MARK: - Multi-Channel Meter View

/// Displays level meters for multiple channels in a horizontal layout
struct ChannelMetersView: View {
    let channelLevels: [ChannelLevelInfo]
    var maxDisplayChannels: Int = 16
    var showChannelLabels: Bool = true

    var body: some View {
        VStack(spacing: 4) {
            ForEach(Array(displayLevels.enumerated()), id: \.offset) { index, levelInfo in
                HStack(spacing: 8) {
                    // Channel label
                    if showChannelLabels {
                        Text("Ch \(levelInfo.id + 1)")
                            .font(.system(size: 10, weight: .medium, design: .monospaced))
                            .foregroundColor(.secondary)
                            .frame(width: 40, alignment: .trailing)
                    }

                    // Level meter
                    LevelMeter(level: levelInfo.level, peak: levelInfo.peak)

                    // dB value
                    Text(formatDB(levelInfo.levelDB))
                        .font(.system(size: 9, weight: .medium, design: .monospaced))
                        .foregroundColor(levelInfo.hasSignal ? .primary : .secondary)
                        .frame(width: 45, alignment: .trailing)
                }
            }

            // Show truncation notice if needed
            if channelLevels.count > maxDisplayChannels {
                Text("+ \(channelLevels.count - maxDisplayChannels) more channels")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .padding(.top, 4)
            }
        }
    }

    private var displayLevels: [ChannelLevelInfo] {
        Array(channelLevels.prefix(maxDisplayChannels))
    }

    private func formatDB(_ db: Float) -> String {
        if db <= -60 {
            return "-inf dB"
        }
        return String(format: "%+.1f dB", db)
    }
}

// MARK: - Compact Vertical Meters

/// Compact vertical meter display for toolbar or sidebar
struct CompactChannelMeters: View {
    let channelLevels: [ChannelLevelInfo]
    var meterHeight: CGFloat = 40

    var body: some View {
        HStack(spacing: 2) {
            ForEach(channelLevels) { levelInfo in
                VerticalLevelMeter(
                    level: levelInfo.level,
                    peak: levelInfo.peak,
                    width: 8,
                    showLabel: channelLevels.count <= 8,
                    channelNumber: levelInfo.id + 1
                )
            }
        }
        .frame(height: meterHeight)
    }
}

// MARK: - Signal Present Indicator

/// Simple indicator showing whether any audio signal is present
struct SignalPresentIndicator: View {
    let hasSignal: Bool
    var size: CGFloat = 10
    var showLabel: Bool = true

    @State private var isPulsing = false

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(hasSignal ? Color.green : Color.gray.opacity(0.5))
                .frame(width: size, height: size)
                .overlay(
                    Circle()
                        .stroke(Color.black.opacity(0.2), lineWidth: 0.5)
                )
                .shadow(color: hasSignal ? Color.green.opacity(0.5) : .clear, radius: isPulsing ? 4 : 2)
                .animation(.easeInOut(duration: 0.5).repeatForever(autoreverses: true), value: isPulsing)
                .onAppear {
                    isPulsing = hasSignal
                }
                .onChange(of: hasSignal) { newValue in
                    isPulsing = newValue
                }

            if showLabel {
                Text(hasSignal ? "Signal" : "No Signal")
                    .font(.caption)
                    .foregroundColor(hasSignal ? .green : .secondary)
            }
        }
    }
}

// MARK: - Stream Audio Status Panel

/// Comprehensive audio status panel for a stream
struct StreamAudioStatusPanel: View {
    let stream: StreamInfo
    @State private var audioLevels: StreamAudioLevels?

    var body: some View {
        GroupBox("Audio Levels") {
            VStack(alignment: .leading, spacing: 12) {
                // Signal indicator
                HStack {
                    SignalPresentIndicator(hasSignal: audioLevels?.hasAnySignal ?? false)

                    Spacer()

                    if let levels = audioLevels {
                        Text("\(levels.activeChannelCount)/\(levels.channelLevels.count) active")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }

                Divider()

                // Channel meters
                if let levels = audioLevels {
                    ChannelMetersView(
                        channelLevels: levels.channelLevels,
                        maxDisplayChannels: 16
                    )
                } else {
                    Text("No audio data available")
                        .font(.caption)
                        .foregroundColor(.secondary)
                        .frame(maxWidth: .infinity, alignment: .center)
                        .padding(.vertical, 20)
                }
            }
            .padding(.vertical, 4)
        }
        .onAppear {
            startLevelUpdates()
        }
    }

    private func startLevelUpdates() {
        // Show static silent meters when no real driver data is available
        // Real implementation would query driver for actual audio levels
        audioLevels = StreamAudioLevels.silentData(
            channelCount: Int(stream.numChannels),
            isConnected: stream.isConnected
        )
    }
}

// MARK: - Legacy Components (maintained for compatibility)

struct AudioStatusIndicator: View {
    @State private var isActive: Bool = false
    @State private var signalLevel: Double = 0.0
    @State private var hasError: Bool = false

    var body: some View {
        HStack {
            // Activity indicator
            Circle()
                .fill(activityColor)
                .frame(width: 12, height: 12)
                .overlay(
                    Circle()
                        .stroke(Color.black.opacity(0.2), lineWidth: 1)
                )

            // Signal meter
            GeometryReader { geometry in
                HStack(spacing: 2) {
                    ForEach(0..<20, id: \.self) { index in
                        Rectangle()
                            .fill(signalSegmentColor(index: index))
                            .opacity(signalSegmentOpacity(index: index))
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .frame(height: 10)
            .cornerRadius(2)
            .overlay(
                RoundedRectangle(cornerRadius: 2)
                    .stroke(Color.gray.opacity(0.3), lineWidth: 1)
            )

            // Error indicator
            if hasError {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundColor(.yellow)
            }
        }
        .onReceive(Timer.publish(every: 0.1, on: .main, in: .common).autoconnect()) { _ in
            updateStatus()
        }
    }

    private var activityColor: Color {
        if hasError {
            return .red
        } else if isActive {
            return .green
        } else {
            return .gray
        }
    }

    private func signalSegmentColor(index: Int) -> Color {
        let threshold = Int(signalLevel * 20)
        if index < Int(signalLevel * 8) {
            return .green
        } else if index < threshold {
            return .yellow
        } else {
            return .gray
        }
    }

    private func signalSegmentOpacity(index: Int) -> Double {
        let threshold = Int(signalLevel * 20)
        return index < threshold ? 1.0 : 0.3
    }

    private func updateStatus() {
        // Static state until real driver integration
        // Real implementation would connect to the driver's status reporting system
        isActive = false
        signalLevel = 0.0
        hasError = false
    }
}

struct AudioStatusPanel: View {
    var body: some View {
        VStack {
            HStack {
                Text("Audio Status")
                    .font(.headline)

                Spacer()

                Text("L")
                    .font(.caption)
                AudioStatusIndicator()

                Text("R")
                    .font(.caption)
                AudioStatusIndicator()
            }
            .padding(.horizontal)

            Divider()
        }
        .frame(maxWidth: .infinity)
    }
}

struct StreamStatusView: View {
    let stream: StreamInfo

    var body: some View {
        HStack {
            VStack(alignment: .leading) {
                Text(stream.name)
                    .font(.headline)

                HStack {
                    Text("Status:")
                    statusBadge
                }

                HStack {
                    Text("Signal:")
                    signalMeter
                }
            }

            Spacer()

            VStack(alignment: .trailing) {
                Text("\(Int((stream.bitrate ?? 0) / 1000)) kbps")
                    .font(.caption)
                    .foregroundColor(.secondary)

                Text(stream.multicastAddress)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
        }
        .padding(.vertical, 4)
    }

    private var statusBadge: some View {
        HStack {
            Circle()
                .fill(streamStatusColor)
                .frame(width: 8, height: 8)
            Text(streamStatusText)
                .font(.caption)
        }
    }

    private var signalMeter: some View {
        GeometryReader { geometry in
            HStack(spacing: 1) {
                ForEach(0..<10, id: \.self) { index in
                    Rectangle()
                        .fill(signalLevelColor(index: index))
                        .opacity(index < signalLevelSegments ? 1.0 : 0.2)
                }
            }
        }
        .frame(height: 6)
        .cornerRadius(3)
    }

    private var streamStatusColor: Color {
        return stream.isConnected ? .green : .red
    }

    private var streamStatusText: String {
        return stream.isConnected ? "Active" : "Inactive"
    }

    private func signalLevelColor(index: Int) -> Color {
        if index < 3 { return .red }
        else if index < 6 { return .yellow }
        else { return .green }
    }

    private var signalLevelSegments: Int {
        return stream.isConnected ? 5 : 0
    }
}

// MARK: - Extension for StreamInfo compatibility

extension StreamInfo {
    var multicastAddress: String {
        return multicastIP
    }

    var bitrate: UInt64? {
        // Calculate estimated bitrate based on format
        let bitsPerSample: UInt64 = encoding == "L24" ? 24 : 16
        return UInt64(sampleRate) * UInt64(numChannels) * bitsPerSample
    }

    var channels: Int {
        return Int(numChannels)
    }
}

// MARK: - Previews

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview("Level Meter") {
    VStack(spacing: 20) {
        LevelMeter(level: 0.3, peak: 0.4)
        LevelMeter(level: 0.6, peak: 0.75)
        LevelMeter(level: 0.85, peak: 0.92)
        LevelMeter(level: 0.99, peak: 1.0)
    }
    .padding()
    .frame(width: 300)
}
#endif

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview("Channel Meters") {
    ChannelMetersView(
        channelLevels: StreamAudioLevels.mockData(channelCount: 8).channelLevels
    )
    .padding()
    .frame(width: 400)
}
#endif

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview("Compact Meters") {
    CompactChannelMeters(
        channelLevels: StreamAudioLevels.mockData(channelCount: 8).channelLevels
    )
    .padding()
    .frame(width: 200)
}
#endif

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview("Signal Indicator") {
    VStack(spacing: 20) {
        SignalPresentIndicator(hasSignal: true)
        SignalPresentIndicator(hasSignal: false)
        SignalPresentIndicator(hasSignal: true, showLabel: false)
    }
    .padding()
}
#endif

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview("Stream Audio Panel") {
    StreamAudioStatusPanel(stream: StreamInfo.example)
        .padding()
        .frame(width: 400)
}
#endif

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview("Legacy Views") {
    VStack {
        AudioStatusPanel()
        Divider()
        StreamStatusView(stream: StreamInfo(
            id: UUID(),
            name: "Test Stream",
            multicastIP: "239.1.2.3",
            port: 5004,
            encoding: "L24",
            sampleRate: 48000,
            numChannels: 2,
            isActive: true
        ))
    }
    .padding()
}
#endif
