//
//  ContentView.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import SwiftUI
import Combine
import UIKit

struct ContentView: View {
    @StateObject private var incomingClipboard = IncomingClipboardViewModel()

    @State private var activePanel: AppPanel?
    @State private var inspectedItem: ClipboardItem?
    @State private var didCheckInitialNetworkKey = false
    @State private var networkIndicatorMode: NetworkIndicatorMode = .ok

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVStack(spacing: 18) {
                    if let item = incomingClipboard.item {
                        Text(item.dayTitle)
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.secondary)
                            .padding(.top, 10)

                        ClipboardGroupView(
                            deviceName: item.deviceName,
                            item: item,
                            isCopied: incomingClipboard.copiedItemID == item.id,
                            onInspect: {
                                inspectedItem = item
                            },
                            onCopy: {
                                incomingClipboard.copy(item)
                            }
                        )
                    } else {
                        EmptyIncomingClipboardView()
                            .padding(.top, 64)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle(CLP_UI_APP_NAME)
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Button {
                        activePanel = .network
                    } label: {
                        NetworkToolbarIndicator(mode: networkIndicatorMode)
                    }
                    .foregroundStyle(.secondary)
                    .accessibilityLabel(CLP_UI_NETWORK)

                    Menu {
                        ForEach(AppPanel.allCases) { panel in
                            Button {
                                activePanel = panel
                            } label: {
                                Label(panel.title, systemImage: panel.symbolName)
                            }
                        }
                    } label: {
                        Image(systemName: "line.3.horizontal")
                    }
                    .accessibilityLabel("Menu")
                }
            }
            .sheet(item: $activePanel) { panel in
                AppPanelSheet(panel: panel)
            }
            .sheet(item: $inspectedItem) { item in
                ClipboardInspectSheet(item: item) {
                    incomingClipboard.copy(item)
                }
            }
            .alert("Unable to Copy", isPresented: incomingClipboard.copyErrorIsPresented) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(incomingClipboard.copyErrorMessage ?? "The clipboard item could not be copied.")
            }
            .task {
                await showNetworkSetupIfNeeded()
            }
            .task {
                await pollNetworkIndicator()
            }
            .task {
                await observeIncomingClipboard()
            }
        }
    }

    private func showNetworkSetupIfNeeded() async {
        guard !didCheckInitialNetworkKey else {
            return
        }

        didCheckInitialNetworkKey = true

        do {
            try NetworkRuntimeBridge.start()
        } catch {
            print("Clipp network runtime failed to start: \(error.localizedDescription)")
        }

        do {
            let status = try await Task.detached(priority: .userInitiated) {
                try NetworkKeyBridge.loadStatus()
            }.value

            if !status.hasNetworkKey && activePanel == nil {
                activePanel = .network
            }
        } catch {
            if activePanel == nil {
                activePanel = .network
            }
        }

        await refreshNetworkIndicator()
    }

    private func pollNetworkIndicator() async {
        while !Task.isCancelled {
            await refreshNetworkIndicator()
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
    }

    private func refreshNetworkIndicator() async {
        let mode = await Task.detached(priority: .utility) {
            guard NetworkRuntimeBridge.isRunning() else {
                return NetworkIndicatorMode.ok
            }

            let hasNetworkKey = (try? NetworkKeyBridge.loadStatus().hasNetworkKey) ?? false
            guard hasNetworkKey else {
                return NetworkIndicatorMode.needsSetup
            }

            return NetworkRuntimeBridge.hasPeerConnections() ? .ok : .searching
        }.value

        networkIndicatorMode = mode
    }

    private func observeIncomingClipboard() async {
        incomingClipboard.refresh()

        let name = Notification.Name(IncomingClipboardBridge.didChangeNotificationName())
        for await _ in NotificationCenter.default.notifications(named: name) {
            incomingClipboard.refresh()
        }
    }
}

@MainActor
private final class IncomingClipboardViewModel: ObservableObject {
    @Published var item: ClipboardItem?
    @Published var copiedItemID: String?
    @Published var copyErrorMessage: String?

    var copyErrorIsPresented: Binding<Bool> {
        Binding(
            get: { self.copyErrorMessage != nil },
            set: { isPresented in
                if !isPresented {
                    self.copyErrorMessage = nil
                }
            }
        )
    }

    func refresh() {
        guard let latest = IncomingClipboardBridge.latestItem() else {
            item = nil
            return
        }

        item = ClipboardItem(sourceItem: latest)
    }

    func copy(_ item: ClipboardItem) {
        do {
            try IncomingClipboardBridge.copy(item.sourceItem)
            copiedItemID = item.id

            Task {
                try? await Task.sleep(nanoseconds: 1_400_000_000)
                await MainActor.run {
                    if self.copiedItemID == item.id {
                        self.copiedItemID = nil
                    }
                }
            }
        } catch {
            copyErrorMessage = error.localizedDescription
        }
    }
}

private enum NetworkIndicatorMode: Sendable {
    case ok
    case searching
    case needsSetup
}

private struct NetworkToolbarIndicator: View {
    let mode: NetworkIndicatorMode

    @State private var sweepRotation = 0.0
    @State private var setupBadgeScale = 1.0

    var body: some View {
        ZStack {
            Image(systemName: "antenna.radiowaves.left.and.right")
                .symbolRenderingMode(.hierarchical)

            if mode == .searching {
                Circle()
                    .trim(from: 0.08, to: 0.28)
                    .stroke(
                        .secondary,
                        style: StrokeStyle(lineWidth: 1.5, lineCap: .round)
                    )
                    .frame(width: 24, height: 24)
                    .rotationEffect(.degrees(sweepRotation))
                    .transition(.opacity)
            }

            if mode == .needsSetup {
                Image(systemName: "exclamationmark.circle.fill")
                    .symbolRenderingMode(.palette)
                    .foregroundStyle(.white, .orange)
                    .font(.system(size: 12, weight: .semibold))
                    .background(
                        Circle()
                            .fill(.background)
                            .frame(width: 13, height: 13)
                    )
                    .scaleEffect(setupBadgeScale)
                    .offset(x: 9, y: -9)
                    .transition(.scale.combined(with: .opacity))
            }
        }
        .frame(width: 28, height: 28)
        .onAppear(perform: updateAnimation)
        .onChange(of: mode) {
            updateAnimation()
        }
    }

    private func updateAnimation() {
        switch mode {
        case .searching:
            setupBadgeScale = 1
            sweepRotation = 0
            withAnimation(.linear(duration: 1.4).repeatForever(autoreverses: false)) {
                sweepRotation = 360
            }
        case .needsSetup:
            sweepRotation = 0
            setupBadgeScale = 1
            withAnimation(.easeInOut(duration: 0.85).repeatForever(autoreverses: true)) {
                setupBadgeScale = 1.18
            }
        case .ok:
            withAnimation(.easeOut(duration: 0.18)) {
                sweepRotation = 0
                setupBadgeScale = 1
            }
        }
    }
}

private enum ClipboardPayload {
    case multilineText(String)
    case oneLineText(String)
    case link(title: String, host: String, url: String)
    case image(Data)
}

private struct ClipboardItem: Identifiable {
    let id: String
    let deviceName: String
    let receivedAt: Date
    let payload: ClipboardPayload
    let sourceItem: IncomingClipboardItem

    var time: String {
        receivedAt.formatted(date: .omitted, time: .shortened)
    }

    var dayTitle: String {
        if Calendar.current.isDateInToday(receivedAt) {
            return "Today"
        }

        return receivedAt.formatted(date: .abbreviated, time: .omitted)
    }

    init?(sourceItem: IncomingClipboardItem) {
        if sourceItem.hasImagePayload, let imagePNGData = sourceItem.imagePNGData {
            payload = .image(imagePNGData)
        } else if sourceItem.hasTextPayload, let text = sourceItem.text {
            payload = Self.classify(text: text)
        } else {
            return nil
        }

        id = sourceItem.identifier
        deviceName = sourceItem.deviceName
        receivedAt = sourceItem.receivedAt
        self.sourceItem = sourceItem
    }

    private static func classify(text: String) -> ClipboardPayload {
        if text.contains("\n") || text.contains("\r") {
            return .multilineText(text)
        }

        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if let url = URL(string: trimmed),
           let scheme = url.scheme?.lowercased(),
           (scheme == "http" || scheme == "https"),
           let host = url.host(percentEncoded: false) {
            return .link(title: host, host: host, url: trimmed)
        }

        return .oneLineText(text)
    }
}

private struct EmptyIncomingClipboardView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray")
                .font(.system(size: 34, weight: .regular))
                .foregroundStyle(.secondary)

            VStack(spacing: 4) {
                Text("No incoming clipboard yet")
                    .font(.headline)

                Text("New clipboard items from trusted devices will appear here.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .frame(maxWidth: 280)
    }
}

private struct ClipboardGroupView: View {
    let deviceName: String
    let item: ClipboardItem
    let isCopied: Bool
    let onInspect: () -> Void
    let onCopy: () -> Void

    var body: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 7) {
                Text(deviceName)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 4)

                ClipboardBubble(
                    item: item,
                    isCopied: isCopied,
                    onInspect: onInspect,
                    onCopy: onCopy
                )
            }
            .frame(maxWidth: 330, alignment: .leading)

            Spacer(minLength: 54)
        }
    }
}

private struct ClipboardBubble: View {
    let item: ClipboardItem
    let isCopied: Bool
    let onInspect: () -> Void
    let onCopy: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ClipboardPayloadView(payload: item.payload)
                .contentShape(Rectangle())
                .onTapGesture(perform: onInspect)

            HStack(spacing: 8) {
                Text(item.time)
                    .font(.caption2)
                    .foregroundStyle(.secondary)

                Spacer(minLength: 12)

                Button(action: onCopy) {
                    Image(systemName: isCopied ? "checkmark" : "doc.on.doc")
                        .font(.caption.weight(.semibold))
                        .frame(width: 24, height: 24)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .accessibilityLabel(isCopied ? "Copied" : "Copy")
            }
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 12)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))
        )
        .overlay {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(.black.opacity(0.05))
        }
    }
}

private struct ClipboardPayloadView: View {
    let payload: ClipboardPayload

    var body: some View {
        switch payload {
        case .multilineText(let text):
            Text(text)
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(.primary)
                .lineSpacing(3)
                .lineLimit(8)
                .textSelection(.enabled)

        case .oneLineText(let text):
            PrivateLineView(text: text)

        case .link(let title, let host, let url):
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 7) {
                    Image(systemName: "link")
                        .font(.caption.weight(.bold))
                    Text(host)
                        .font(.caption.weight(.semibold))
                }
                .foregroundStyle(.secondary)

                Text(title)
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(.primary)

                Text(url)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .textSelection(.enabled)

        case .image(let data):
            ClipboardImagePreview(data: data)
        }
    }
}

private struct ClipboardImagePreview: View {
    let data: Data

    var body: some View {
        if let image = UIImage(data: data) {
            Image(uiImage: image)
                .resizable()
                .scaledToFill()
                .frame(width: 250, height: 170)
                .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
        } else {
            Label("Image preview unavailable", systemImage: "photo")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }
}

private struct PrivateLineView: View {
    let text: String

    @State private var isPeeking = false

    private var maskedText: String {
        String(repeating: "•", count: min(max(text.count, 10), 28))
    }

    var body: some View {
        HStack(spacing: 9) {
            Text(isPeeking ? text : maskedText)
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(.primary)
                .lineLimit(1)
                .truncationMode(.middle)

            Image(systemName: isPeeking ? "eye.fill" : "eye")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
        }
        .contentShape(Rectangle())
        .onLongPressGesture(
            minimumDuration: 0.28,
            maximumDistance: 50,
            pressing: { isPressing in
                withAnimation(.easeOut(duration: 0.12)) {
                    isPeeking = isPressing
                }
            },
            perform: {}
        )
        .accessibilityLabel(isPeeking ? text : "Hidden one-line clipboard item")
    }
}

private struct ClipboardInspectSheet: View {
    let item: ClipboardItem
    let onCopy: () -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text(item.deviceName)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)

                    ClipboardInspectPayloadView(payload: item.payload)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(16)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle(item.time)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Done") {
                        dismiss()
                    }
                }

                ToolbarItem(placement: .topBarTrailing) {
                    Button(action: onCopy) {
                        Image(systemName: "doc.on.doc")
                    }
                    .accessibilityLabel("Copy")
                }
            }
        }
    }
}

private struct ClipboardInspectPayloadView: View {
    let payload: ClipboardPayload

    var body: some View {
        switch payload {
        case .multilineText(let text):
            Text(text)
                .font(.system(.body, design: .monospaced))
                .lineSpacing(4)
                .textSelection(.enabled)

        case .oneLineText(let text):
            PrivateLineView(text: text)
                .padding(.vertical, 4)

        case .link(let title, let host, let url):
            VStack(alignment: .leading, spacing: 12) {
                VStack(alignment: .leading, spacing: 5) {
                    HStack(spacing: 7) {
                        Image(systemName: "link")
                            .font(.caption.weight(.bold))
                        Text(host)
                            .font(.caption.weight(.semibold))
                    }
                    .foregroundStyle(.secondary)

                    Text(title)
                        .font(.headline)

                    Text(url)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }

                if let destination = URL(string: url) {
                    Link(destination: destination) {
                        Label("Open", systemImage: "safari")
                    }
                    .buttonStyle(.bordered)
                }
            }

        case .image(let data):
            if let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            } else {
                Label("Image preview unavailable", systemImage: "photo")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

#Preview {
    ContentView()
}
