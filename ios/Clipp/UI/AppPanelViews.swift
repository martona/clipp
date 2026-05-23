//
//  AppPanelViews.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import SwiftUI
import Combine

enum AppPanel: String, CaseIterable, Identifiable {
    case network
    case settings
    case logs
    case about

    var id: String { rawValue }

    var title: String {
        switch self {
        case .network:
            CLP_UI_NETWORK
        case .settings:
            CLP_UI_SETTINGS
        case .logs:
            CLP_UI_LOGS
        case .about:
            CLP_UI_ABOUT
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
    @StateObject private var model = NetworkKeyViewModel()
    @State private var confirmingReplacement = false

    var body: some View {
        Form {
            Section(CLP_UI_NETWORK_KEY) {
                TextField("Network Name", text: $model.networkName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                SecureField(CLP_UI_SECRET, text: $model.secret)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit {
                        submitKey()
                    }

                if model.shouldShowAction {
                    Button {
                        submitKey()
                    } label: {
                        Label(model.actionTitle, systemImage: "key")
                    }
                } else {
                    NetworkKeyStatusCard(model: model)
                }
            }
            .alert("Recreate Network Key?", isPresented: $confirmingReplacement) {
                Button("Cancel", role: .cancel) {}
                Button("Recreate", role: .destructive) {
                    model.deriveAndStoreKey()
                }
            } message: {
                Text("Peers using the old network key will stop connecting until they are updated with the same network name and secret.")
            }
            .onChange(of: model.secret) {
                model.updateStatusMessage()
            }
            .onChange(of: model.networkName) {
                model.networkNameDidChange()
            }

            if model.hasNetworkKey {
                Section("Peer Status") {
                    PeerStatusRow(deviceName: "MacBook Pro", status: "Nearby", isOnline: true)
                    PeerStatusRow(deviceName: "Studio PC", status: "Last seen 3 min ago", isOnline: false)
                }
            } else {
                Section {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("No network key configured")
                            .font(.subheadline.weight(.semibold))

                        Text("Create one here to connect this iPhone with your other Clipp devices.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .task {
            model.loadStatus()
        }
    }

    private func submitKey() {
        guard model.canCreateKey else {
            return
        }

        if model.shouldConfirmReplacement {
            confirmingReplacement = true
        } else {
            model.deriveAndStoreKey()
        }
    }
}

@MainActor
private final class NetworkKeyViewModel: ObservableObject {
    @Published var networkName = ""
    @Published var secret = ""
    @Published var fingerprint: String?
    @Published var hasNetworkKey = false
    @Published var isWorking = false
    @Published var statusMessage = "Loading network key status..."
    @Published var statusIsError = false

    private var storedNetworkName = ""
    private var keyClearedForNetworkNameChange = false
    private var networkNameUpdateGeneration = 0

    var actionTitle: String {
        hasNetworkKey ? "Recreate Network Key" : "Create Network Key"
    }

    var canCreateKey: Bool {
        !isWorking && !networkName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && secret.count >= 8
    }

    var shouldConfirmReplacement: Bool {
        hasNetworkKey
    }

    var shouldShowAction: Bool {
        canCreateKey
    }

    var shouldShowFingerprint: Bool {
        hasNetworkKey
            && fingerprint != nil
            && secret.isEmpty
            && networkName == storedNetworkName
            && !isWorking
            && !statusIsError
    }

    func loadStatus() {
        do {
            apply(status: try NetworkKeyBridge.loadStatus())
        } catch {
            fingerprint = nil
            hasNetworkKey = false
            statusIsError = true
            statusMessage = error.localizedDescription
        }
    }

    func deriveAndStoreKey() {
        guard canCreateKey else {
            updateStatusMessage()
            return
        }

        let requestedNetworkName = networkName.trimmingCharacters(in: .whitespacesAndNewlines)
        let requestedSecret = secret
        isWorking = true
        statusIsError = false
        statusMessage = CLP_UI_WORKING

        Task {
            do {
                let status = try await Task.detached(priority: .userInitiated) {
                    try NetworkKeyBridge.deriveAndStoreKey(networkName: requestedNetworkName, secret: requestedSecret)
                }.value

                isWorking = false
                secret = ""
                apply(status: status)
            } catch {
                isWorking = false
                fingerprint = nil
                statusIsError = true
                statusMessage = error.localizedDescription
            }
        }
    }

    func updateStatusMessage() {
        if isWorking {
            statusIsError = false
            statusMessage = CLP_UI_WORKING
        } else if networkName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            statusIsError = true
            statusMessage = "Network name cannot be empty."
        } else if !secret.isEmpty && secret.count < 8 {
            statusIsError = true
            statusMessage = CLP_UI_SECRET_TOO_SHORT
        } else if !secret.isEmpty {
            statusIsError = false
            statusMessage = "Ready to derive and store a network key."
        } else if keyClearedForNetworkNameChange {
            statusIsError = false
            statusMessage = "Enter the network secret again after changing the network name."
        } else if hasNetworkKey && networkName != storedNetworkName {
            statusIsError = false
            statusMessage = "Enter the network secret again after changing the network name."
        } else if hasNetworkKey {
            statusIsError = false
            statusMessage = CLP_UI_NETWORK_KEY_FINGERPRINT
        } else {
            statusIsError = false
            statusMessage = CLP_UI_ENTER_NETWORK_SECRET
        }
    }

    func networkNameDidChange() {
        updateStatusMessage()

        let requestedNetworkName = networkName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !requestedNetworkName.isEmpty, requestedNetworkName != storedNetworkName else {
            return
        }

        networkNameUpdateGeneration += 1
        let generation = networkNameUpdateGeneration
        let hadNetworkKey = hasNetworkKey
        if hadNetworkKey {
            fingerprint = nil
            hasNetworkKey = false
            keyClearedForNetworkNameChange = true
            updateStatusMessage()
        }

        Task {
            try? await Task.sleep(nanoseconds: 300_000_000)
            guard generation == networkNameUpdateGeneration else {
                return
            }

            do {
                let status = try await Task.detached(priority: .utility) {
                    try NetworkKeyBridge.updateNetworkName(requestedNetworkName)
                }.value

                guard generation == networkNameUpdateGeneration,
                      networkName.trimmingCharacters(in: .whitespacesAndNewlines) == status.networkName else {
                    return
                }

                apply(status: status)
                if hadNetworkKey {
                    keyClearedForNetworkNameChange = true
                    updateStatusMessage()
                }
            } catch {
                guard generation == networkNameUpdateGeneration else {
                    return
                }
                fingerprint = nil
                hasNetworkKey = false
                statusIsError = true
                statusMessage = error.localizedDescription
            }
        }
    }

    private func apply(status: NetworkKeyStatus) {
        networkName = status.networkName
        storedNetworkName = status.networkName
        fingerprint = status.fingerprint
        hasNetworkKey = status.hasNetworkKey
        if status.hasNetworkKey {
            keyClearedForNetworkNameChange = false
        }
        statusIsError = false
        updateStatusMessage()
    }
}

private struct NetworkKeyStatusCard: View {
    @ObservedObject var model: NetworkKeyViewModel

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: model.shouldShowFingerprint ? "key.fill" : "info.circle")
                .foregroundStyle(model.statusIsError ? .red : .secondary)
                .frame(width: 22)

            VStack(alignment: .leading, spacing: 4) {
                if model.shouldShowFingerprint, let fingerprint = model.fingerprint {
                    Text(fingerprint)
                        .font(.system(.subheadline, design: .monospaced).weight(.semibold))
                        .textSelection(.enabled)
                }

                Text(model.statusMessage)
                    .font(.footnote)
                    .foregroundStyle(model.statusIsError ? .red : .secondary)
            }
        }
        .padding(.vertical, 4)
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
                        Text(CLP_UI_ABOUT_TITLE)
                            .font(.title2.weight(.semibold))

                        Text(CLP_UI_TAGLINE)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .padding(.top, 4)
                }

                VStack(alignment: .leading, spacing: 10) {
                    Text(CLP_UI_PROJECT)
                        .font(.headline)

                    Text(CLP_UI_COPYRIGHT)
                    Text(CLP_UI_MIT_LICENSE)
                    Link(CLP_UI_REPOSITORY_LABEL, destination: URL(string: CLP_UI_REPOSITORY_URL)!)
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)

                VStack(alignment: .leading, spacing: 10) {
                    Text(CLP_UI_OPEN_SOURCE_ACKNOWLEDGEMENTS)
                        .font(.headline)
                        .foregroundStyle(.primary)

                    Text(CLP_UI_ACK_LIBSODIUM)
                    Text(CLP_UI_ACK_LODEPNG)
                    Text(CLP_UI_ACK_XXHASH)
                    Text(CLP_UI_ACK_ZSTD)
                }
                .font(.subheadline)
                .foregroundStyle(.secondary)

                Text(CLP_UI_THIRD_PARTY_LICENSE_NOTE)
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
