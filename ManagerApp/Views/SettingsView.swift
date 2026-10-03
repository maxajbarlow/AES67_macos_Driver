//
// SettingsView.swift
// AES67 Manager
// Application settings and preferences
//

import SwiftUI
import Network

struct SettingsView: View {
    @EnvironmentObject var driverManager: DriverManager
    @AppStorage("autoRefresh") private var autoRefresh = true
    @AppStorage("refreshInterval") private var refreshInterval = 1.0

    var body: some View {
        TabView {
            GeneralSettings()
                .environmentObject(driverManager)
                .tabItem {
                    Label("General", systemImage: "gear")
                }

            AudioSettings()
                .environmentObject(driverManager)
                .tabItem {
                    Label("Audio", systemImage: "speaker.wave.2")
                }

            NetworkSettings()
                .tabItem {
                    Label("Network", systemImage: "network")
                }

            DriverSettings()
                .environmentObject(driverManager)
                .tabItem {
                    Label("Driver", systemImage: "waveform")
                }

            AboutSettings()
                .tabItem {
                    Label("About", systemImage: "info.circle")
                }
        }
        .frame(width: 550, height: 450)
    }
}

struct GeneralSettings: View {
    @EnvironmentObject var driverManager: DriverManager
    @AppStorage("autoRefresh") private var autoRefresh = true
    @AppStorage("refreshInterval") private var refreshInterval = 1.0
    @AppStorage("showDetailedStats") private var showDetailedStats = true
    @AppStorage("showChannelViz") private var showChannelViz = true

    var body: some View {
        Form {
            Section("Status Updates") {
                Toggle("Auto-refresh stream status", isOn: $autoRefresh)

                if autoRefresh {
                    HStack {
                        Text("Refresh interval")
                        Spacer()
                        Slider(value: $refreshInterval, in: 0.5...5.0, step: 0.5)
                            .frame(width: 200)
                            .onChange(of: refreshInterval) { newValue in
                                driverManager.updateRefreshInterval(newValue)
                            }
                        Text("\(refreshInterval, specifier: "%.1f")s")
                            .frame(width: 40)
                    }
                }
            }

            Section("Appearance") {
                Toggle("Show detailed statistics", isOn: $showDetailedStats)
                Toggle("Show channel visualization", isOn: $showChannelViz)
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}

// MARK: - Audio Settings

struct AudioSettings: View {
    @EnvironmentObject var driverManager: DriverManager

    var body: some View {
        Form {
            Section("Device Sample Rate") {
                Picker("Sample Rate", selection: Binding(
                    get: { driverManager.currentDeviceSampleRate },
                    set: { driverManager.setDeviceSampleRate($0) }
                )) {
                    ForEach(DriverManager.supportedSampleRates, id: \.self) { rate in
                        Text(DriverManager.formatSampleRate(rate)).tag(rate)
                    }
                }
                .pickerStyle(.menu)

                HStack {
                    Text("Current device rate")
                    Spacer()
                    Text(DriverManager.formatSampleRate(driverManager.currentDeviceSampleRate))
                        .foregroundColor(.secondary)
                }
            }

            Section("Buffer") {
                HStack {
                    Text("Buffer size")
                    Spacer()
                    Text("Managed by driver")
                        .foregroundColor(.secondary)
                }
            }

            Section("Format Info") {
                HStack {
                    Text("Supported encodings")
                    Spacer()
                    Text("L16, L24")
                        .foregroundColor(.secondary)
                }

                HStack {
                    Text("Max channels")
                    Spacer()
                    Text("128")
                        .foregroundColor(.secondary)
                }

                HStack {
                    Text("Channels in use")
                    Spacer()
                    Text("\(driverManager.totalChannelsUsed)")
                        .foregroundColor(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}

// MARK: - Network Settings

struct NetworkSettings: View {
    @AppStorage("preferredInterface") private var preferredInterface = "auto"
    @AppStorage("defaultMulticastTTL") private var defaultMulticastTTL = 32
    @AppStorage("defaultPort") private var defaultPort = 5004
    @State private var availableInterfaces: [String] = []

    var body: some View {
        Form {
            Section("Network Interface") {
                Picker("Preferred interface", selection: $preferredInterface) {
                    Text("Automatic").tag("auto")
                    ForEach(availableInterfaces, id: \.self) { iface in
                        Text(iface).tag(iface)
                    }
                }
                .pickerStyle(.menu)
            }

            Section("Multicast Defaults") {
                HStack {
                    Text("Default TTL")
                    Spacer()
                    Stepper("\(defaultMulticastTTL)", value: $defaultMulticastTTL, in: 1...255)
                        .frame(width: 120)
                }

                HStack {
                    Text("Default port")
                    Spacer()
                    TextField("", value: $defaultPort, format: .number)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 80)
                }
            }
        }
        .formStyle(.grouped)
        .padding()
        .onAppear {
            availableInterfaces = getNetworkInterfaces()
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

            if family == UInt8(AF_INET) {
                let name = String(cString: interface.ifa_name)
                if !name.hasPrefix("lo") && !interfaces.contains(name) {
                    interfaces.append(name)
                }
            }
        }

        return interfaces.sorted()
    }
}

// MARK: - Driver Settings

struct DriverSettings: View {
    @EnvironmentObject var driverManager: DriverManager

    private var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "Unknown"
    }

    private var buildNumber: String {
        Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "Unknown"
    }

    var body: some View {
        Form {
            Section("Driver Status") {
                HStack {
                    Text("Status")
                    Spacer()
                    StatusBadge(isConnected: driverManager.isDriverLoaded)
                }

                HStack {
                    Text("Version")
                    Spacer()
                    Text(appVersion)
                        .foregroundColor(.secondary)
                }

                HStack {
                    Text("Location")
                    Spacer()
                    Text("/Library/Audio/Plug-Ins/HAL/")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }

            Section("Actions") {
                Button("Restart Core Audio") {
                    driverManager.restartCoreAudio()
                }

                Button("Check Driver Installation") {
                    driverManager.checkDriverInstallation()
                }
            }
        }
        .formStyle(.grouped)
        .padding()
    }
}

// MARK: - About Settings

struct AboutSettings: View {
    private var appVersion: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "Unknown"
    }

    private var buildNumber: String {
        Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "Unknown"
    }

    private var copyrightYear: String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy"
        return formatter.string(from: Date())
    }

    var body: some View {
        VStack(spacing: 20) {
            Image(systemName: "waveform.circle.fill")
                .font(.system(size: 64))
                .foregroundColor(.blue)

            Text("AES67 Audio Driver")
                .font(.title)
                .fontWeight(.bold)

            Text("Version \(appVersion) (Build #\(buildNumber))")
                .foregroundColor(.secondary)

            Divider()
                .padding(.horizontal, 60)

            VStack(alignment: .leading, spacing: 12) {
                Text("Professional AES67/RAVENNA/Dante audio driver for macOS")
                    .multilineTextAlignment(.center)

                Text("Features:")
                    .fontWeight(.semibold)

                VStack(alignment: .leading, spacing: 4) {
                    Label("128 audio channels", systemImage: "speaker.wave.3")
                    Label("L16 and L24 encoding", systemImage: "waveform")
                    Label("PTP synchronization", systemImage: "clock")
                    Label("Multiple simultaneous streams", systemImage: "network")
                }
                .font(.caption)
            }
            .padding()

            Spacer()

            Text("\u{00A9} \(copyrightYear) AES67 Driver Project")
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

#if DEBUG  // previews need Xcode: plain swiftc (build.sh) has no PreviewsMacros plugin
#Preview {
    SettingsView()
        .environmentObject(DriverManager())
}
#endif
