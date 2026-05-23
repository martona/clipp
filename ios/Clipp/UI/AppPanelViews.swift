//
//  AppPanelViews.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import SwiftUI

enum AppPanel: String, CaseIterable, Identifiable {
    case network
    case settings
    case logs
    case about

    var id: String { rawValue }

    var title: String {
        switch self {
        case .network:
            "Network"
        case .settings:
            "Settings"
        case .logs:
            "Logs"
        case .about:
            "About"
        }
    }

    var symbolName: String {
        switch self {
        case .network:
            "antenna.radiowaves.left.and.right"
        case .settings:
            "gearshape"
        case .logs:
            "terminal"
        case .about:
            "info.circle"
        }
    }
}

struct AppPanelSheet: View {
    let panel: AppPanel

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                switch panel {
                case .network:
                    NetworkPanelView()
                case .settings:
                    SettingsPanelView()
                case .logs:
                    LogsPanelView()
                case .about:
                    AboutPanelView()
                }
            }
            .navigationTitle(panel.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") {
                        dismiss()
                    }
                }
            }
        }
    }
}

private struct NetworkPanelView: View {
    var body: some View {
        Form {
            Section("Network Key") {
                LabeledContent("Status", value: "Not configured")

                Button {
                } label: {
                    Label("Set Up Network Key", systemImage: "key")
                }
                .disabled(true)
            }

            Section("Peer Status") {
                PeerStatusRow(deviceName: "MacBook Pro", status: "Nearby", isOnline: true)
                PeerStatusRow(deviceName: "Studio PC", status: "Last seen 3 min ago", isOnline: false)
            }
        }
    }
}

private struct SettingsPanelView: View {
    var body: some View {
        Form {
            Section("Clipboard") {
                Toggle("Receive from trusted devices", isOn: .constant(true))
                    .disabled(true)
                Toggle("Send copied text from this iPhone", isOn: .constant(true))
                    .disabled(true)
                Toggle("Collapse one-line items", isOn: .constant(true))
                    .disabled(true)
            }

            Section("Notifications") {
                Toggle("Notify when peers connect", isOn: .constant(false))
                    .disabled(true)
            }
        }
    }
}

private struct LogsPanelView: View {
    private let lines = [
        "09:41:02 discovery started",
        "09:41:04 peer MacBook Pro reachable",
        "09:42:18 clipboard item received",
        "09:45:31 clipboard item queued for send"
    ]

    var body: some View {
        List {
            Section("Recent") {
                ForEach(lines, id: \.self) { line in
                    Text(line)
                        .font(.system(.footnote, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

private struct AboutPanelView: View {
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                HStack(alignment: .top, spacing: 18) {
                    Image("ClippAboutArtwork")
                        .resizable()
                        .scaledToFit()
                        .frame(width: 92, height: 92)
                        .accessibilityHidden(true)

                    VStack(alignment: .leading, spacing: 6) {
                        Text("Clipp 1.0")
                            .font(.title2.weight(.semibold))

                        Text("Secure cross-platform clipboard sync for trusted devices")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.top, 4)
                }

                VStack(alignment: .leading, spacing: 10) {
                    Text("Project")
                        .font(.headline)

                    Text("Copyright (C) 2026 Marton Anka")
                    Text("Released under the MIT License.")
                    Link("github.com/martona/clipp", destination: URL(string: "https://github.com/martona/clipp")!)
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)

                VStack(alignment: .leading, spacing: 10) {
                    Text("Open Source Acknowledgements")
                        .font(.headline)
                        .foregroundStyle(.primary)

                    Text("libsodium - ISC-licensed cryptography library")
                    Text("lodepng - zlib-licensed PNG encoder/decoder")
                    Text("xxHash - BSD-2-Clause non-cryptographic hashing")
                    Text("Zstandard (zstd) - BSD-licensed compression")
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)

                Text("Third-party license terms remain with their respective projects.")
                    .font(.footnote)
                    .foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(24)
        }
        .background(Color(.systemGroupedBackground))
    }
}

private struct PeerStatusRow: View {
    let deviceName: String
    let status: String
    let isOnline: Bool

    var body: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(isOnline ? Color.green : Color.secondary.opacity(0.42))
                .frame(width: 10, height: 10)

            VStack(alignment: .leading, spacing: 2) {
                Text(deviceName)
                Text(status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}
