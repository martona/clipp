//
//  AppPanelViews.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import SwiftUI
import Combine
import UIKit

enum AppPanel: String, Identifiable {
    case network
    case settings
    case about
    case diagnostics

    var id: String { rawValue }

    static let allCases: [AppPanel] = [.network, .settings, .about]

    var title: String {
        switch self {
        case .network:
            CLP_UI_NETWORK
        case .settings:
            CLP_UI_SETTINGS
        case .about:
            CLP_UI_ABOUT
        case .diagnostics:
            CLP_UI_DIAGNOSTICS
        }
    }

    var symbolName: String {
        switch self {
        case .network:
            "antenna.radiowaves.left.and.right"
        case .settings:
            "gearshape"
        case .about:
            "info.circle"
        case .diagnostics:
            "terminal"
        }
    }
}

struct AppPanelSheet: View {
    let panel: AppPanel
    let openPanel: (AppPanel) -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                switch panel {
                case .network:
                    NetworkPanelView()
                case .settings:
                    SettingsPanelView()
                case .about:
                    AboutPanelView {
                        openPanel(.diagnostics)
                    }
                case .diagnostics:
                    DiagnosticsPanelView()
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
    @StateObject private var model = SettingsViewModel()
    @State private var confirmingHostIDReset = false

    var body: some View {
        Form {
            Section {
                HistoryLimitSlider(
                    title: CLP_UI_HISTORY_MEMORY_LIMIT,
                    stops: SettingsLimitStop.memoryStops,
                    selection: $model.historyMemoryIndex
                ) {
                    model.applyClipboardHistorySettings()
                }

                HistoryLimitSlider(
                    title: CLP_UI_HISTORY_TIME_LIMIT,
                    stops: SettingsLimitStop.ageStops,
                    selection: $model.historyAgeIndex
                ) {
                    model.applyClipboardHistorySettings()
                }

                HistoryLimitSlider(
                    title: CLP_UI_HISTORY_ITEM_LIMIT,
                    stops: SettingsLimitStop.itemStops,
                    selection: $model.historyItemIndex
                ) {
                    model.applyClipboardHistorySettings()
                }
            } header: {
                Text(CLP_UI_CLIPBOARD_HISTORY)
            } footer: {
                if model.historyStatusMessage != nil {
                    SettingsStatusText(message: model.historyStatusMessage, isError: model.historyStatusIsError)
                }
            }

            Section {
                SettingsTextFieldRow(
                    title: CLP_UI_TCP_PORT,
                    text: $model.tcpPort,
                    keyboardType: .numberPad
                ) {
                    model.applyNetworkSettings()
                }

                SettingsTextFieldRow(
                    title: CLP_UI_UDP_PORT,
                    text: $model.udpPort,
                    keyboardType: .numberPad
                ) {
                    model.applyNetworkSettings()
                }

                SettingsTextFieldRow(
                    title: CLP_UI_LISTENER_IP,
                    text: $model.listenerIP,
                    keyboardType: .numbersAndPunctuation
                ) {
                    model.applyNetworkSettings()
                }

                SettingsTextFieldRow(
                    title: CLP_UI_MULTICAST_IP,
                    text: $model.multicastIP,
                    keyboardType: .numbersAndPunctuation
                ) {
                    model.applyNetworkSettings()
                }

                Button {
                    model.applyNetworkSettings()
                } label: {
                    Label(CLP_UI_APPLY_NETWORK_SETTINGS, systemImage: "arrow.clockwise")
                }
                .disabled(!model.canApplyNetworkSettings)
            } header: {
                Text(CLP_UI_NETWORK)
            } footer: {
                if model.networkStatusMessage != nil {
                    SettingsStatusText(message: model.networkStatusMessage, isError: model.networkStatusIsError)
                }
            }

            Section {
                VStack(alignment: .leading, spacing: 6) {
                    Text(CLP_UI_CURRENT_HOST_ID)
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Text(model.hostID.isEmpty ? CLP_UI_UNAVAILABLE : model.hostID)
                        .font(.system(.footnote, design: .monospaced))
                        .textSelection(.enabled)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                .padding(.vertical, 2)

                Button(role: .destructive) {
                    confirmingHostIDReset = true
                } label: {
                    Label(CLP_UI_RESET, systemImage: "arrow.counterclockwise")
                }

                if model.hasHostIDCollisionWarning {
                    Label(CLP_UI_HOST_ID_COLLISION_WARNING, systemImage: "exclamationmark.triangle.fill")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                }
            } header: {
                Text(CLP_UI_HOST_ID)
            } footer: {
                if model.hostIDStatusMessage != nil {
                    SettingsStatusText(message: model.hostIDStatusMessage, isError: model.hostIDStatusIsError)
                }
            }
        }
        .task {
            model.load()
        }
        .alert("Reset Host ID?", isPresented: $confirmingHostIDReset) {
            Button("Cancel", role: .cancel) {}
            Button(CLP_UI_RESET, role: .destructive) {
                model.resetHostID()
            }
        } message: {
            Text(CLP_UI_HOST_ID_COLLISION_WARNING)
        }
    }
}

private struct SettingsTextFieldRow: View {
    let title: String
    @Binding var text: String
    let keyboardType: UIKeyboardType
    let onSubmit: () -> Void

    var body: some View {
        LabeledContent {
            TextField(title, text: $text)
                .keyboardType(keyboardType)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .multilineTextAlignment(.trailing)
                .onSubmit {
                    onSubmit()
                }
        } label: {
            Text(title)
        }
    }
}

private struct SettingsLimitStop: Identifiable {
    let value: UInt64
    let label: String

    var id: String {
        "\(value)-\(label)"
    }

    private static let mib: UInt64 = 1024 * 1024
    private static let gib: UInt64 = 1024 * mib

    static let memoryStops = [
        SettingsLimitStop(value: 1 * mib, label: "1 MB"),
        SettingsLimitStop(value: 8 * mib, label: "8 MB"),
        SettingsLimitStop(value: 32 * mib, label: "32 MB"),
        SettingsLimitStop(value: 128 * mib, label: "128 MB"),
        SettingsLimitStop(value: 256 * mib, label: "256 MB"),
        SettingsLimitStop(value: 512 * mib, label: "512 MB"),
        SettingsLimitStop(value: 1 * gib, label: "1 GB"),
        SettingsLimitStop(value: 2 * gib, label: "2 GB"),
        SettingsLimitStop(value: 0, label: CLP_UI_UNLIMITED)
    ]

    static let ageStops = [
        SettingsLimitStop(value: 1, label: "1 second"),
        SettingsLimitStop(value: 10, label: "10 seconds"),
        SettingsLimitStop(value: 60, label: "1 minute"),
        SettingsLimitStop(value: 10 * 60, label: "10 minutes"),
        SettingsLimitStop(value: 60 * 60, label: "1 hour"),
        SettingsLimitStop(value: 6 * 60 * 60, label: "6 hours"),
        SettingsLimitStop(value: 24 * 60 * 60, label: "1 day"),
        SettingsLimitStop(value: 7 * 24 * 60 * 60, label: "7 days"),
        SettingsLimitStop(value: 30 * 24 * 60 * 60, label: "30 days"),
        SettingsLimitStop(value: 0, label: CLP_UI_UNLIMITED)
    ]

    static let itemStops = [
        SettingsLimitStop(value: 1, label: "1 item"),
        SettingsLimitStop(value: 10, label: "10 items"),
        SettingsLimitStop(value: 50, label: "50 items"),
        SettingsLimitStop(value: 100, label: "100 items"),
        SettingsLimitStop(value: 500, label: "500 items"),
        SettingsLimitStop(value: 1000, label: "1000 items"),
        SettingsLimitStop(value: 5000, label: "5000 items"),
        SettingsLimitStop(value: 10000, label: "10000 items"),
        SettingsLimitStop(value: 0, label: CLP_UI_UNLIMITED)
    ]
}

private struct HistoryLimitSlider: View {
    let title: String
    let stops: [SettingsLimitStop]
    @Binding var selection: Double
    let onChange: () -> Void

    private var selectedLabel: String {
        guard !stops.isEmpty else {
            return ""
        }

        let index = min(max(Int(selection.rounded()), 0), stops.count - 1)
        return stops[index].label
    }

    private var sliderSelection: Binding<Double> {
        Binding(
            get: {
                selection
            },
            set: { newValue in
                selection = newValue.rounded()
                onChange()
            }
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title)
                Spacer()
                Text(selectedLabel)
                    .font(.footnote.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            Slider(
                value: sliderSelection,
                in: 0...Double(max(stops.count - 1, 0)),
                step: 1
            )
        }
        .padding(.vertical, 4)
    }
}

@MainActor
private final class SettingsViewModel: ObservableObject {
    @Published var historyMemoryIndex = 0.0
    @Published var historyAgeIndex = 0.0
    @Published var historyItemIndex = 0.0
    @Published var tcpPort = ""
    @Published var udpPort = ""
    @Published var listenerIP = ""
    @Published var multicastIP = ""
    @Published var hostID = ""
    @Published var hasHostIDCollisionWarning = false
    @Published var historyStatusMessage: String?
    @Published var historyStatusIsError = false
    @Published var networkStatusMessage: String?
    @Published var networkStatusIsError = false
    @Published var hostIDStatusMessage: String?
    @Published var hostIDStatusIsError = false

    private var loadingSnapshot = false
    private var storedTcpPort = ""
    private var storedUdpPort = ""
    private var storedListenerIP = ""
    private var storedMulticastIP = ""

    var canApplyNetworkSettings: Bool {
        !loadingSnapshot && (
            tcpPort.trimmingCharacters(in: .whitespacesAndNewlines) != storedTcpPort ||
            udpPort.trimmingCharacters(in: .whitespacesAndNewlines) != storedUdpPort ||
            listenerIP.trimmingCharacters(in: .whitespacesAndNewlines) != storedListenerIP ||
            multicastIP.trimmingCharacters(in: .whitespacesAndNewlines) != storedMulticastIP
        )
    }

    func load() {
        do {
            apply(snapshot: try SettingsBridge.loadSnapshot())
            clearStatusMessages()
        } catch {
            networkStatusIsError = true
            networkStatusMessage = error.localizedDescription
        }
    }

    func applyClipboardHistorySettings() {
        guard !loadingSnapshot else {
            return
        }

        do {
            let snapshot = try SettingsBridge.updateClipboardHistory(
                memoryLimitBytes: selectedValue(SettingsLimitStop.memoryStops, historyMemoryIndex),
                maxAgeSeconds: selectedValue(SettingsLimitStop.ageStops, historyAgeIndex),
                maxItems: selectedValue(SettingsLimitStop.itemStops, historyItemIndex)
            )
            apply(snapshot: snapshot)
            historyStatusIsError = false
            historyStatusMessage = CLP_UI_CLIPBOARD_HISTORY_SETTINGS_APPLIED
        } catch {
            historyStatusIsError = true
            historyStatusMessage = error.localizedDescription
        }
    }

    func applyNetworkSettings() {
        let parsedTcpPort = parsePort(tcpPort)
        let parsedUdpPort = parsePort(udpPort)

        guard let parsedTcpPort, let parsedUdpPort else {
            networkStatusIsError = true
            networkStatusMessage = "Ports must be between 1 and 65535."
            return
        }

        do {
            let snapshot = try SettingsBridge.updateNetwork(
                tcpPort: parsedTcpPort,
                udpPort: parsedUdpPort,
                listenerIP: listenerIP,
                multicastIP: multicastIP
            )
            apply(snapshot: snapshot)
            networkStatusIsError = false
            networkStatusMessage = CLP_UI_NETWORK_SETTINGS_APPLIED
        } catch {
            networkStatusIsError = true
            networkStatusMessage = error.localizedDescription
        }
    }

    func resetHostID() {
        do {
            apply(snapshot: try SettingsBridge.resetHostID())
            hostIDStatusIsError = false
            hostIDStatusMessage = CLP_UI_HOST_ID_RESET
        } catch {
            hostIDStatusIsError = true
            hostIDStatusMessage = error.localizedDescription
        }
    }

    private func apply(snapshot: SettingsSnapshot) {
        loadingSnapshot = true
        historyMemoryIndex = Double(stopIndex(SettingsLimitStop.memoryStops, snapshot.clipboardHistoryMemoryLimitBytes))
        historyAgeIndex = Double(stopIndex(SettingsLimitStop.ageStops, snapshot.clipboardHistoryMaxAgeSeconds))
        historyItemIndex = Double(stopIndex(SettingsLimitStop.itemStops, snapshot.clipboardHistoryMaxItems))

        storedTcpPort = String(snapshot.tcpPort)
        storedUdpPort = String(snapshot.udpPort)
        storedListenerIP = snapshot.listenerIP
        storedMulticastIP = snapshot.multicastIP
        tcpPort = storedTcpPort
        udpPort = storedUdpPort
        listenerIP = storedListenerIP
        multicastIP = storedMulticastIP
        hostID = snapshot.hostID
        hasHostIDCollisionWarning = snapshot.hasHostIDCollisionWarning
        loadingSnapshot = false
    }

    private func clearStatusMessages() {
        historyStatusMessage = nil
        networkStatusMessage = nil
        hostIDStatusMessage = nil
    }

    private func parsePort(_ text: String) -> Int? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let value = Int(trimmed), value >= 1, value <= 65535 else {
            return nil
        }
        return value
    }

    private func selectedValue(_ stops: [SettingsLimitStop], _ index: Double) -> UInt64 {
        guard !stops.isEmpty else {
            return 0
        }

        return stops[min(max(Int(index.rounded()), 0), stops.count - 1)].value
    }

    private func stopIndex(_ stops: [SettingsLimitStop], _ value: UInt64) -> Int {
        if let exact = stops.firstIndex(where: { $0.value == value }) {
            return exact
        }

        for index in stops.indices.dropLast() where value <= stops[index].value {
            return index
        }

        return max(stops.count - 1, 0)
    }
}

private struct SettingsStatusText: View {
    let message: String?
    let isError: Bool

    var body: some View {
        if let message {
            Text(message)
                .foregroundStyle(isError ? .red : .secondary)
        }
    }
}

private struct DiagnosticsPanelView: View {
    @StateObject private var model = DiagnosticLogViewModel()

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(CLP_UI_LIVE_DIAGNOSTIC_OUTPUT)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)

                Spacer(minLength: 8)

                Button(model.copyTitle) {
                    model.copy()
                }
                .disabled(model.lines.isEmpty)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 12)

            DiagnosticsTextView(lines: model.lines)
                .ignoresSafeArea(edges: .bottom)
        }
        .background(Color(.systemGroupedBackground))
    }
}

@MainActor
private final class DiagnosticLogViewModel: ObservableObject {
    @Published var lines: [DiagnosticLogLine] = []
    @Published private var plainText: String = ""

    private var observer: AnyCancellable?

    var copyTitle: String {
        String(format: CLP_UI_COPY_LOG_LINES_FORMAT, lines.count)
    }

    init() {
        reload()
        observer = NotificationCenter.default.publisher(
            for: Notification.Name(DiagnosticLogsBridge.didChangeNotificationName())
        )
        .receive(on: RunLoop.main)
        .sink { [weak self] _ in
            self?.reload()
        }
    }

    func copy() {
        UIPasteboard.general.string = plainText
    }

    private func reload() {
        lines = DiagnosticLogsBridge.snapshot()
        plainText = DiagnosticLogsBridge.plainText()
    }
}

private struct DiagnosticsTextView: UIViewRepresentable {
    let lines: [DiagnosticLogLine]

    func makeUIView(context: Context) -> UITextView {
        let textView = UITextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.isScrollEnabled = true
        textView.alwaysBounceVertical = true
        textView.backgroundColor = UIColor(white: 0.06, alpha: 1.0)
        textView.textContainerInset = UIEdgeInsets(top: 10, left: 10, bottom: 10, right: 10)
        textView.textContainer.lineFragmentPadding = 0
        textView.font = UIFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        textView.textColor = Self.color(forRawValue: 0)
        return textView
    }

    func updateUIView(_ textView: UITextView, context: Context) {
        let shouldFollow = Self.isNearBottom(textView)
        textView.attributedText = Self.attributedText(for: lines)
        if shouldFollow {
            DispatchQueue.main.async {
                Self.scrollToBottom(textView)
            }
        }
    }

    private static func attributedText(for lines: [DiagnosticLogLine]) -> NSAttributedString {
        let text = NSMutableAttributedString()
        let font = UIFont.monospacedSystemFont(ofSize: 12, weight: .regular)

        for line in lines {
            for run in line.runs {
                text.append(NSAttributedString(
                    string: run.text,
                    attributes: [
                        .font: font,
                        .foregroundColor: color(forRawValue: run.color.rawValue)
                    ]
                ))
            }
            text.append(NSAttributedString(
                string: "\n",
                attributes: [
                    .font: font,
                    .foregroundColor: color(forRawValue: 0)
                ]
            ))
        }

        return text
    }

    private static func color(forRawValue rawValue: Int) -> UIColor {
        switch rawValue {
        case 1:
            return UIColor(white: 0.50, alpha: 1.0)
        case 2:
            return UIColor(red: 0.24, green: 0.58, blue: 0.68, alpha: 1.0)
        case 3:
            return UIColor(red: 0.33, green: 0.78, blue: 0.92, alpha: 1.0)
        case 4:
            return UIColor(red: 0.40, green: 0.86, blue: 0.45, alpha: 1.0)
        case 5:
            return UIColor(red: 0.95, green: 0.76, blue: 0.25, alpha: 1.0)
        case 6:
            return UIColor(red: 1.00, green: 0.36, blue: 0.25, alpha: 1.0)
        default:
            return UIColor(white: 0.82, alpha: 1.0)
        }
    }

    private static func isNearBottom(_ textView: UITextView) -> Bool {
        textView.layoutIfNeeded()
        let visibleHeight = textView.bounds.height - textView.adjustedContentInset.top - textView.adjustedContentInset.bottom
        let distanceFromBottom = textView.contentSize.height - visibleHeight - textView.contentOffset.y
        return distanceFromBottom <= 48
    }

    private static func scrollToBottom(_ textView: UITextView) {
        guard textView.attributedText.length > 0 else {
            return
        }

        textView.scrollRangeToVisible(NSRange(location: textView.attributedText.length, length: 0))
    }
}

private struct AboutPanelView: View {
    let openDiagnostics: () -> Void

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

                Button {
                    openDiagnostics()
                } label: {
                    Label(CLP_UI_DIAGNOSTICS, systemImage: "terminal")
                }
                .buttonStyle(.bordered)
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
