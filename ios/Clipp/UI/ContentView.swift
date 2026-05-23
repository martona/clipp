//
//  ContentView.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import SwiftUI

struct ContentView: View {
    private let groups = ClipboardGroup.sampleGroups

    @State private var activePanel: AppPanel?
    @State private var didCheckInitialNetworkKey = false
    @State private var networkIndicatorMode: NetworkIndicatorMode = .ok

    var body: some View {
        NavigationStack {
            ScrollView {
                LazyVStack(spacing: 18) {
                    Text("Today")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .padding(.top, 10)

                    ForEach(groups) { group in
                        ClipboardGroupView(group: group)
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
            .task {
                await showNetworkSetupIfNeeded()
            }
            .task {
                await pollNetworkIndicator()
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

private enum ClipboardDirection {
    case incoming
    case outgoing
}

private enum ClipboardPayload {
    case multilineText(String)
    case oneLineText(String)
    case link(title: String, host: String, url: String)
}

private struct ClipboardItem: Identifiable {
    let id = UUID()
    let time: String
    let payload: ClipboardPayload
}

private struct ClipboardGroup: Identifiable {
    let id = UUID()
    let direction: ClipboardDirection
    let deviceName: String
    let items: [ClipboardItem]

    static let sampleGroups = [
        ClipboardGroup(
            direction: .incoming,
            deviceName: "MacBook Pro",
            items: [
                ClipboardItem(
                    time: "9:41 AM",
                    payload: .multilineText(
                        """
                        Build notes
                        - macOS bundle signs cleanly
                        - iOS shell is running
                        - Next: shared transport layer
                        """
                    )
                ),
                ClipboardItem(
                    time: "9:42 AM",
                    payload: .link(
                        title: "Clipp project board",
                        host: "github.com",
                        url: "https://github.com/martona/clipp"
                    )
                )
            ]
        ),
        ClipboardGroup(
            direction: .outgoing,
            deviceName: "This iPhone",
            items: [
                ClipboardItem(
                    time: "9:45 AM",
                    payload: .oneLineText("net.clipp.ios")
                ),
                ClipboardItem(
                    time: "9:46 AM",
                    payload: .multilineText(
                        """
                        func copyToNearbyDevices(_ text: String) {
                            clipboard.send(text)
                        }
                        """
                    )
                )
            ]
        ),
        ClipboardGroup(
            direction: .incoming,
            deviceName: "Studio PC",
            items: [
                ClipboardItem(
                    time: "9:48 AM",
                    payload: .oneLineText("sk-live-83a6f042f24c4e6bb4e997")
                )
            ]
        )
    ]
}

private struct ClipboardGroupView: View {
    let group: ClipboardGroup

    private var alignment: HorizontalAlignment {
        group.direction == .incoming ? .leading : .trailing
    }

    private var frameAlignment: Alignment {
        group.direction == .incoming ? .leading : .trailing
    }

    var body: some View {
        HStack(alignment: .top) {
            if group.direction == .outgoing {
                Spacer(minLength: 54)
            }

            VStack(alignment: alignment, spacing: 7) {
                Text(group.deviceName)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 4)

                ForEach(group.items) { item in
                    ClipboardBubble(item: item, direction: group.direction)
                }
            }
            .frame(maxWidth: 330, alignment: frameAlignment)

            if group.direction == .incoming {
                Spacer(minLength: 54)
            }
        }
    }
}

private struct ClipboardBubble: View {
    let item: ClipboardItem
    let direction: ClipboardDirection

    private var isOutgoing: Bool {
        direction == .outgoing
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ClipboardPayloadView(payload: item.payload, isOutgoing: isOutgoing)

            Text(item.time)
                .font(.caption2)
                .foregroundStyle(isOutgoing ? .white.opacity(0.62) : .secondary)
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 12)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(isOutgoing ? Color.clippInk : Color(.secondarySystemGroupedBackground))
        )
        .overlay {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(isOutgoing ? .white.opacity(0.10) : .black.opacity(0.05))
        }
    }
}

private struct ClipboardPayloadView: View {
    let payload: ClipboardPayload
    let isOutgoing: Bool

    var body: some View {
        switch payload {
        case .multilineText(let text):
            Text(text)
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(isOutgoing ? .white : .primary)
                .lineSpacing(3)
                .textSelection(.enabled)

        case .oneLineText(let text):
            PrivateLineView(text: text, isOutgoing: isOutgoing)

        case .link(let title, let host, let url):
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 7) {
                    Image(systemName: "link")
                        .font(.caption.weight(.bold))
                    Text(host)
                        .font(.caption.weight(.semibold))
                }
                .foregroundStyle(isOutgoing ? .white.opacity(0.72) : .secondary)

                Text(title)
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(isOutgoing ? .white : .primary)

                Text(url)
                    .font(.caption)
                    .foregroundStyle(isOutgoing ? .white.opacity(0.72) : .secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .textSelection(.enabled)
        }
    }
}

private struct PrivateLineView: View {
    let text: String
    let isOutgoing: Bool

    @State private var isPeeking = false

    private var maskedText: String {
        String(repeating: "•", count: min(max(text.count, 10), 28))
    }

    var body: some View {
        HStack(spacing: 9) {
            Text(isPeeking ? text : maskedText)
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(isOutgoing ? .white : .primary)
                .lineLimit(1)
                .truncationMode(.middle)

            Image(systemName: isPeeking ? "eye.fill" : "eye")
                .font(.caption.weight(.semibold))
                .foregroundStyle(isOutgoing ? .white.opacity(0.72) : .secondary)
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

private extension Color {
    static let clippInk = Color(red: 0.0, green: 15.0 / 255.0, blue: 54.0 / 255.0)
}

#Preview {
    ContentView()
}
