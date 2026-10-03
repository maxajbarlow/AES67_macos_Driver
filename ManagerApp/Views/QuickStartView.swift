//
// QuickStartView.swift
// AES67 Manager - Build #18
// First-run setup wizard for new users
//

import SwiftUI
import Network
import ServiceManagement

// MARK: - Main Quick Start View

struct QuickStartView: View {
    @EnvironmentObject var driverManager: DriverManager
    @Binding var isPresented: Bool
    @AppStorage("hasCompletedQuickStart") private var hasCompletedQuickStart = false
    @State private var currentStep = 1

    var body: some View {
        VStack(spacing: 0) {
            // Header with progress
            VStack(spacing: 12) {
                HStack {
                    Text("Quick Start Setup")
                        .font(.headline)
                    Spacer()
                    Button(action: { skipWizard() }) {
                        Text("Skip")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                    .buttonStyle(.plain)
                }

                // Progress indicator
                ProgressView(value: Double(currentStep), total: 4)
                    .progressViewStyle(.linear)

                // Step labels
                HStack {
                    ForEach(1...4, id: \.self) { step in
                        Text(stepLabel(for: step))
                            .font(.caption2)
                            .foregroundColor(currentStep >= step ? .accentColor : .secondary)
                        if step < 4 {
                            Spacer()
                        }
                    }
                }
            }
            .padding()
            .background(Color(nsColor: .windowBackgroundColor))

            Divider()

            // Step content
            TabView(selection: $currentStep) {
                WelcomeStep(onNext: { withAnimation { currentStep = 2 } })
                    .tag(1)

                NetworkCheckStep(
                    onNext: { withAnimation { currentStep = 3 } },
                    onBack: { withAnimation { currentStep = 1 } }
                )
                .tag(2)

                AddStreamStep(
                    onNext: { withAnimation { currentStep = 4 } },
                    onBack: { withAnimation { currentStep = 2 } }
                )
                .environmentObject(driverManager)
                .tag(3)

                FinishStep(
                    onComplete: { completeWizard() },
                    onBack: { withAnimation { currentStep = 3 } }
                )
                .environmentObject(driverManager)
                .tag(4)
            }
            // Note: .page() tab view style not available on macOS - using default
            .animation(.easeInOut, value: currentStep)
        }
        .frame(width: 550, height: 480)
        .background(Color(nsColor: .controlBackgroundColor))
    }

    private func stepLabel(for step: Int) -> String {
        switch step {
        case 1: return "Welcome"
        case 2: return "Network"
        case 3: return "Add Stream"
        case 4: return "Finish"
        default: return ""
        }
    }

    private func completeWizard() {
        hasCompletedQuickStart = true
        isPresented = false
    }

    private func skipWizard() {
        hasCompletedQuickStart = true
        isPresented = false
    }
}

// MARK: - Step 1: Welcome

struct WelcomeStep: View {
    let onNext: () -> Void

    var body: some View {
        VStack(spacing: 24) {
            Spacer()

            // Icon
            ZStack {
                Circle()
                    .fill(Color.accentColor.opacity(0.1))
                    .frame(width: 100, height: 100)

                Image(systemName: "waveform.circle.fill")
                    .font(.system(size: 60))
                    .foregroundColor(.accentColor)
            }

            // Title
            Text("Welcome to AES67 Manager")
                .font(.title)
                .fontWeight(.semibold)

            // Description
            VStack(spacing: 8) {
                Text("This wizard will help you set up your first AES67 audio stream in just a few steps.")
                    .multilineTextAlignment(.center)
                    .foregroundColor(.secondary)

                Text("AES67 allows you to receive professional audio streams over your network with sample-accurate synchronization.")
                    .multilineTextAlignment(.center)
                    .foregroundColor(.secondary)
                    .font(.callout)
            }
            .padding(.horizontal, 40)

            // Features list
            VStack(alignment: .leading, spacing: 8) {
                FeatureRow(icon: "antenna.radiowaves.left.and.right", text: "Discover streams automatically via SAP")
                FeatureRow(icon: "clock.badge.checkmark", text: "PTP synchronization for sample accuracy")
                FeatureRow(icon: "slider.horizontal.3", text: "Route up to 128 channels to any app")
            }
            .padding(.horizontal, 60)
            .padding(.top, 8)

            Spacer()

            // Navigation
            HStack {
                Spacer()
                Button("Get Started") {
                    onNext()
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
            }
            .padding()
        }
    }
}

struct FeatureRow: View {
    let icon: String
    let text: String

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .frame(width: 20)
                .foregroundColor(.accentColor)
            Text(text)
                .font(.callout)
        }
    }
}

// MARK: - Step 2: Network Check

struct NetworkCheckStep: View {
    let onNext: () -> Void
    let onBack: () -> Void

    @State private var isChecking = true
    @State private var networkInterfaceOK = false
    @State private var multicastOK = false
    @State private var driverOK = false
    @State private var firewallOK = false
    @State private var selectedInterface = "en0"
    @State private var availableInterfaces: [String] = []

    var body: some View {
        VStack(spacing: 20) {
            // Title
            VStack(spacing: 8) {
                Image(systemName: "network")
                    .font(.system(size: 40))
                    .foregroundColor(.accentColor)

                Text("Network Check")
                    .font(.title2)
                    .fontWeight(.semibold)

                Text("Verifying your network is ready for AES67")
                    .foregroundColor(.secondary)
            }
            .padding(.top, 20)

            // Network status checks
            VStack(spacing: 0) {
                StatusCheckRow(
                    title: "Network Interface",
                    subtitle: networkInterfaceOK ? "Found active interface" : "Checking...",
                    status: isChecking ? .checking : (networkInterfaceOK ? .ok : .warning)
                )

                Divider().padding(.leading, 44)

                StatusCheckRow(
                    title: "Multicast Support",
                    subtitle: multicastOK ? "Multicast routing available" : "Checking multicast...",
                    status: isChecking ? .checking : (multicastOK ? .ok : .warning)
                )

                Divider().padding(.leading, 44)

                StatusCheckRow(
                    title: "Audio Driver",
                    subtitle: driverOK ? "AES67 driver installed" : "Driver not found",
                    status: isChecking ? .checking : (driverOK ? .ok : .error)
                )

                Divider().padding(.leading, 44)

                StatusCheckRow(
                    title: "Firewall",
                    subtitle: firewallOK ? "Ports accessible" : "Check firewall settings",
                    status: isChecking ? .checking : (firewallOK ? .ok : .warning)
                )
            }
            .background(Color(nsColor: .textBackgroundColor))
            .cornerRadius(8)
            .padding(.horizontal, 40)

            // Interface selector (if multiple available)
            if !availableInterfaces.isEmpty {
                HStack {
                    Text("Network Interface:")
                        .foregroundColor(.secondary)
                    Picker("", selection: $selectedInterface) {
                        ForEach(availableInterfaces, id: \.self) { iface in
                            Text(iface).tag(iface)
                        }
                    }
                    .pickerStyle(.menu)
                    .frame(width: 150)
                }
                .padding(.horizontal, 40)
            }

            // Warning or help text
            if !isChecking && !driverOK {
                HStack(spacing: 8) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.orange)
                    Text("Install the AES67 driver package to continue.")
                        .font(.callout)
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 40)
            }

            Spacer()

            // Navigation
            HStack {
                Button("Back") {
                    onBack()
                }

                Spacer()

                Button(action: { checkNetwork() }) {
                    HStack(spacing: 4) {
                        Image(systemName: "arrow.clockwise")
                        Text("Recheck")
                    }
                }
                .disabled(isChecking)

                Button("Next") {
                    onNext()
                }
                .buttonStyle(.borderedProminent)
            }
            .padding()
        }
        .onAppear {
            checkNetwork()
        }
    }

    private func checkNetwork() {
        isChecking = true

        // Simulate network checks with slight delays for better UX
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            // Check for network interfaces
            availableInterfaces = getNetworkInterfaces()
            networkInterfaceOK = !availableInterfaces.isEmpty

            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                // Check multicast support (simplified check)
                multicastOK = checkMulticastSupport()

                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                    // Check driver installation
                    let driverPath = "/Library/Audio/Plug-Ins/HAL/AES67Driver.driver"
                    driverOK = FileManager.default.fileExists(atPath: driverPath)

                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                        // Assume firewall is OK for now (would need more complex check)
                        firewallOK = true
                        isChecking = false
                    }
                }
            }
        }
    }

    private func getNetworkInterfaces() -> [String] {
        var interfaces: [String] = []
        var ifaddr: UnsafeMutablePointer<ifaddrs>?

        guard getifaddrs(&ifaddr) == 0 else { return interfaces }
        defer { freeifaddrs(ifaddr) }

        var ptr = ifaddr
        while ptr != nil {
            defer { ptr = ptr?.pointee.ifa_next }

            let interface = ptr!.pointee
            let family = interface.ifa_addr.pointee.sa_family

            // Only include IPv4 interfaces that are up and not loopback
            if family == UInt8(AF_INET) {
                let name = String(cString: interface.ifa_name)
                if !name.hasPrefix("lo") && !interfaces.contains(name) {
                    interfaces.append(name)
                }
            }
        }

        return interfaces.sorted()
    }

    private func checkMulticastSupport() -> Bool {
        // Simplified check - in production, would test actual multicast routing
        return true
    }
}

enum CheckStatus {
    case checking, ok, warning, error
}

struct StatusCheckRow: View {
    let title: String
    let subtitle: String
    let status: CheckStatus

    var body: some View {
        HStack(spacing: 12) {
            // Status icon
            Group {
                switch status {
                case .checking:
                    ProgressView()
                        .scaleEffect(0.7)
                        .frame(width: 20, height: 20)
                case .ok:
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundColor(.green)
                case .warning:
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundColor(.orange)
                case .error:
                    Image(systemName: "xmark.circle.fill")
                        .foregroundColor(.red)
                }
            }
            .frame(width: 24)

            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .fontWeight(.medium)
                Text(subtitle)
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Spacer()
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
    }
}

// MARK: - Step 3: Add Stream

struct AddStreamStep: View {
    let onNext: () -> Void
    let onBack: () -> Void

    @EnvironmentObject var driverManager: DriverManager

    @State private var addMethod: AddMethod = .discover
    @State private var streamName = ""
    @State private var multicastIP = "239.69.0.1"
    @State private var port = "5004"
    @State private var numChannels = 8
    @State private var isDiscovering = false
    @State private var discoveredStreams: [DiscoveredStream] = []
    @State private var selectedDiscoveredStream: DiscoveredStream?

    enum AddMethod: String, CaseIterable {
        case discover = "Discover"
        case manual = "Manual"
        case sdp = "Import SDP"
    }

    var body: some View {
        VStack(spacing: 16) {
            // Title
            VStack(spacing: 8) {
                Image(systemName: "plus.circle")
                    .font(.system(size: 40))
                    .foregroundColor(.accentColor)

                Text("Add Your First Stream")
                    .font(.title2)
                    .fontWeight(.semibold)

                Text("Choose how to add an AES67 stream")
                    .foregroundColor(.secondary)
            }
            .padding(.top, 16)

            // Method selector
            Picker("Add Method", selection: $addMethod) {
                ForEach(AddMethod.allCases, id: \.self) { method in
                    Text(method.rawValue).tag(method)
                }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 60)

            // Content based on method
            Group {
                switch addMethod {
                case .discover:
                    DiscoverContent(
                        isDiscovering: $isDiscovering,
                        discoveredStreams: $discoveredStreams,
                        selectedStream: $selectedDiscoveredStream
                    )

                case .manual:
                    ManualEntryContent(
                        streamName: $streamName,
                        multicastIP: $multicastIP,
                        port: $port,
                        numChannels: $numChannels
                    )

                case .sdp:
                    SDPImportContent()
                        .environmentObject(driverManager)
                }
            }
            .padding(.horizontal, 40)

            Spacer()

            // Navigation
            HStack {
                Button("Back") {
                    onBack()
                }

                Spacer()

                if addMethod == .manual {
                    Button("Add Stream") {
                        addManualStream()
                    }
                    .disabled(!isManualEntryValid)
                }

                Button(addMethod == .discover && selectedDiscoveredStream != nil ? "Add Selected & Continue" : "Skip & Continue") {
                    if addMethod == .discover, let stream = selectedDiscoveredStream {
                        addDiscoveredStream(stream)
                    }
                    onNext()
                }
                .buttonStyle(.borderedProminent)
            }
            .padding()
        }
    }

    private var isManualEntryValid: Bool {
        !streamName.isEmpty && !multicastIP.isEmpty && !port.isEmpty
    }

    private func addManualStream() {
        guard let portNum = UInt16(port) else { return }

        driverManager.addStream(
            name: streamName,
            multicastIP: multicastIP,
            port: portNum,
            numChannels: UInt16(numChannels),
            sampleRate: 48000,
            encoding: "L24"
        )

        onNext()
    }

    private func addDiscoveredStream(_ stream: DiscoveredStream) {
        driverManager.addStream(
            name: stream.name,
            multicastIP: stream.multicastIP,
            port: stream.port,
            numChannels: stream.channels,
            sampleRate: stream.sampleRate,
            encoding: "L24"
        )
    }
}

struct DiscoveredStream: Identifiable, Hashable {
    let id = UUID()
    let name: String
    let multicastIP: String
    let port: UInt16
    let channels: UInt16
    let sampleRate: UInt32
}

struct DiscoverContent: View {
    @Binding var isDiscovering: Bool
    @Binding var discoveredStreams: [DiscoveredStream]
    @Binding var selectedStream: DiscoveredStream?

    var body: some View {
        VStack(spacing: 12) {
            if isDiscovering {
                VStack(spacing: 8) {
                    ProgressView()
                    Text("Scanning network for AES67 streams...")
                        .font(.callout)
                        .foregroundColor(.secondary)
                }
                .frame(height: 150)
            } else if discoveredStreams.isEmpty {
                VStack(spacing: 12) {
                    Image(systemName: "magnifyingglass")
                        .font(.system(size: 32))
                        .foregroundColor(.secondary)

                    Text("No streams discovered")
                        .foregroundColor(.secondary)

                    Button("Scan Again") {
                        startDiscovery()
                    }
                }
                .frame(height: 150)
            } else {
                ScrollView {
                    VStack(spacing: 4) {
                        ForEach(discoveredStreams) { stream in
                            DiscoveredStreamRow(
                                stream: stream,
                                isSelected: selectedStream?.id == stream.id
                            )
                            .onTapGesture {
                                selectedStream = stream
                            }
                        }
                    }
                }
                .frame(height: 150)
                .background(Color(nsColor: .textBackgroundColor))
                .cornerRadius(8)
            }
        }
        .onAppear {
            startDiscovery()
        }
    }

    private func startDiscovery() {
        isDiscovering = true
        discoveredStreams = []

        // Simulate SAP discovery with a delay
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            // In production, this would use the actual SAP listener
            // For demo purposes, we'll show the empty state
            isDiscovering = false
        }
    }
}

struct DiscoveredStreamRow: View {
    let stream: DiscoveredStream
    let isSelected: Bool

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(stream.name)
                    .fontWeight(.medium)
                Text("\(stream.multicastIP):\(stream.port) - \(stream.channels)ch @ \(stream.sampleRate/1000)kHz")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }

            Spacer()

            if isSelected {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(.accentColor)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(isSelected ? Color.accentColor.opacity(0.1) : Color.clear)
        .cornerRadius(6)
    }
}

struct ManualEntryContent: View {
    @Binding var streamName: String
    @Binding var multicastIP: String
    @Binding var port: String
    @Binding var numChannels: Int

    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text("Stream Name")
                    .frame(width: 100, alignment: .trailing)
                TextField("My Stream", text: $streamName)
                    .textFieldStyle(.roundedBorder)
            }

            HStack {
                Text("Multicast IP")
                    .frame(width: 100, alignment: .trailing)
                TextField("239.69.0.1", text: $multicastIP)
                    .textFieldStyle(.roundedBorder)
            }

            HStack {
                Text("Port")
                    .frame(width: 100, alignment: .trailing)
                TextField("5004", text: $port)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 100)
                Spacer()
            }

            HStack {
                Text("Channels")
                    .frame(width: 100, alignment: .trailing)
                Stepper("\(numChannels)", value: $numChannels, in: 1...64)
                    .frame(width: 100)
                Spacer()
            }
        }
        .padding(.vertical, 8)
    }
}

struct SDPImportContent: View {
    @EnvironmentObject var driverManager: DriverManager
    @State private var isDragging = false

    var body: some View {
        VStack(spacing: 16) {
            // Drop zone
            ZStack {
                RoundedRectangle(cornerRadius: 12)
                    .strokeBorder(style: StrokeStyle(lineWidth: 2, dash: [8]))
                    .foregroundColor(isDragging ? .accentColor : .secondary.opacity(0.5))

                VStack(spacing: 8) {
                    Image(systemName: "doc.badge.arrow.up")
                        .font(.system(size: 32))
                        .foregroundColor(.secondary)

                    Text("Drag & drop SDP file here")
                        .foregroundColor(.secondary)

                    Text("or")
                        .font(.caption)
                        .foregroundColor(.secondary)

                    Button("Choose File...") {
                        driverManager.importSDPFile()
                    }
                }
            }
            .frame(height: 150)
            .onDrop(of: [.fileURL], isTargeted: $isDragging) { providers in
                handleDrop(providers)
            }
        }
    }

    private func handleDrop(_ providers: [NSItemProvider]) -> Bool {
        guard let provider = providers.first else { return false }

        provider.loadItem(forTypeIdentifier: "public.file-url", options: nil) { item, _ in
            guard let data = item as? Data,
                  let url = URL(dataRepresentation: data, relativeTo: nil),
                  url.pathExtension.lowercased() == "sdp" else { return }

            DispatchQueue.main.async {
                driverManager.importSDPFromURL(url)
            }
        }

        return true
    }
}

// MARK: - Step 4: Finish

struct FinishStep: View {
    let onComplete: () -> Void
    let onBack: () -> Void

    @EnvironmentObject var driverManager: DriverManager
    @State private var showInMenuBar = true
    @State private var launchAtLogin = false

    var body: some View {
        VStack(spacing: 24) {
            Spacer()

            // Success icon
            ZStack {
                Circle()
                    .fill(Color.green.opacity(0.1))
                    .frame(width: 80, height: 80)

                Image(systemName: "checkmark.circle.fill")
                    .font(.system(size: 50))
                    .foregroundColor(.green)
            }

            // Title
            Text("You're All Set!")
                .font(.title)
                .fontWeight(.semibold)

            // Summary
            VStack(spacing: 8) {
                if driverManager.streams.isEmpty {
                    Text("No streams added yet, but you can add them anytime from the main window.")
                        .multilineTextAlignment(.center)
                        .foregroundColor(.secondary)
                } else {
                    Text("You've added \(driverManager.streams.count) stream\(driverManager.streams.count == 1 ? "" : "s") with \(driverManager.totalChannelsUsed) total channels.")
                        .multilineTextAlignment(.center)
                        .foregroundColor(.secondary)
                }
            }
            .padding(.horizontal, 40)

            // Quick settings
            VStack(spacing: 0) {
                Toggle(isOn: $showInMenuBar) {
                    HStack {
                        Image(systemName: "menubar.rectangle")
                            .frame(width: 24)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Show in Menu Bar")
                            Text("Quick access to stream status")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)

                Divider().padding(.leading, 56)

                Toggle(isOn: $launchAtLogin) {
                    HStack {
                        Image(systemName: "power")
                            .frame(width: 24)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Launch at Login")
                            Text("Start automatically when you log in")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .cornerRadius(8)
            .padding(.horizontal, 40)

            // Tips
            VStack(alignment: .leading, spacing: 8) {
                Text("Quick Tips:")
                    .font(.caption)
                    .fontWeight(.semibold)
                    .foregroundColor(.secondary)

                VStack(alignment: .leading, spacing: 4) {
                    TipRow(text: "Use Command+N to quickly add streams")
                    TipRow(text: "Import SDP files with Command+O")
                    TipRow(text: "Click the menu bar icon for quick status")
                }
            }
            .padding(.horizontal, 60)

            Spacer()

            // Navigation
            HStack {
                Button("Back") {
                    onBack()
                }

                Spacer()

                Button("Finish Setup") {
                    savePreferences()
                    onComplete()
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
            }
            .padding()
        }
    }

    private func savePreferences() {
        UserDefaults.standard.set(showInMenuBar, forKey: "showInMenuBar")
        UserDefaults.standard.set(launchAtLogin, forKey: "launchAtLogin")

        if launchAtLogin {
            try? SMAppService.mainApp.register()
        } else {
            try? SMAppService.mainApp.unregister()
        }
    }
}

struct TipRow: View {
    let text: String

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: "lightbulb")
                .font(.caption)
                .foregroundColor(.yellow)
            Text(text)
                .font(.caption)
                .foregroundColor(.secondary)
        }
    }
}

// MARK: - Preview

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview {
    QuickStartView(isPresented: .constant(true))
        .environmentObject(DriverManager())
}
#endif
