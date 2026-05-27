//
//  ContentView.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import SwiftUI
import Combine
import UIKit
import ImageIO

struct ContentView: View {
    @StateObject private var clipboardStream = ClipboardStreamViewModel()

    @State private var activePanel: AppPanel?
    @State private var inspectedItem: ClipboardStreamItem?
    @State private var didCheckInitialNetworkKey = false
    @State private var networkIndicatorMode: NetworkIndicatorMode = .ok
    @State private var networkTrafficState = NetworkTrafficState()

    var body: some View {
        NavigationStack {
            ZStack(alignment: .bottom) {
                ScrollView {
                    LazyVStack(spacing: 18) {
                        if !clipboardStream.items.isEmpty {
                            Text(clipboardStream.items.first?.dayTitle ?? "Recent")
                                .font(.caption.weight(.semibold))
                                .foregroundStyle(.secondary)
                                .padding(.top, 10)

                            ForEach(clipboardStream.items) { item in
                                ClipboardGroupView(
                                    item: item,
                                    isCopied: clipboardStream.copiedItemID == item.id,
                                    onInspect: {
                                        inspectedItem = item
                                    },
                                    onCopy: {
                                        clipboardStream.copy(item)
                                    }
                                )
                            }
                        } else {
                            EmptyClipboardActivityView()
                                .padding(.top, 64)
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.bottom, 96)
                }

                Button {
                    clipboardStream.send()
                } label: {
                    SendBottomButton(state: clipboardStream.sendState)
                }
                .disabled(clipboardStream.sendState == .sending)
                .padding(.bottom, 18)
                .accessibilityLabel("Send Clipboard")
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle(CLP_UI_APP_NAME)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button {
                        activePanel = .network
                    } label: {
                        NetworkToolbarIndicator(
                            mode: networkIndicatorMode,
                            sendingUntil: networkTrafficState.sendingUntil,
                            receivingUntil: networkTrafficState.receivingUntil
                        )
                    }
                    .foregroundStyle(.secondary)
                    .accessibilityLabel(CLP_UI_NETWORK)
                }

                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        ForEach(AppPanel.allCases) { panel in
                            Button {
                                activePanel = panel
                            } label: {
                                Label(panel.title, systemImage: panel.symbolName)
                            }
                        }
                    } label: {
                        Image(systemName: "ellipsis")
                            .frame(width: 28, height: 28)
                    }
                    .accessibilityLabel("Menu")
                }
            }
            .sheet(item: $activePanel) { panel in
                AppPanelSheet(panel: panel) { destination in
                    activePanel = destination
                }
            }
            .sheet(item: $inspectedItem) { item in
                ClipboardInspectSheet(item: item) {
                    clipboardStream.copy(item)
                }
            }
            .alert("Unable to Copy", isPresented: clipboardStream.copyErrorIsPresented) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(clipboardStream.copyErrorMessage ?? "The clipboard item could not be copied.")
            }
            .alert("Unable to Send", isPresented: clipboardStream.sendErrorIsPresented) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(clipboardStream.sendErrorMessage ?? "The clipboard could not be sent.")
            }
            .task {
                await showNetworkSetupIfNeeded()
            }
            .task {
                await pollNetworkIndicator()
            }
            .task {
                await pollNetworkTraffic()
            }
            .task {
                await observeClipboardActivity()
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

    private func pollNetworkTraffic() async {
        while !Task.isCancelled {
            refreshNetworkTraffic()
            try? await Task.sleep(nanoseconds: 450_000_000)
        }
    }

    private func refreshNetworkTraffic() {
        let snapshot = NetworkRuntimeBridge.trafficSnapshot()
        networkTrafficState.observe(
            bytesSent: snapshot.bytesSent,
            bytesReceived: snapshot.bytesReceived,
            now: Date()
        )
    }

    private func observeClipboardActivity() async {
        clipboardStream.refreshActivity()

        let name = Notification.Name(ClipboardActivityBridge.didChangeNotificationName())
        for await _ in NotificationCenter.default.notifications(named: name) {
            clipboardStream.refreshActivity()
        }
    }
}

private enum OutgoingSendState: Sendable {
    case idle
    case sending
    case sent
}

@MainActor
private final class ClipboardStreamViewModel: ObservableObject {
    @Published var items: [ClipboardStreamItem] = []
    @Published var copiedItemID: String?
    @Published var copyErrorMessage: String?
    @Published var sendState: OutgoingSendState = .idle
    @Published var sendErrorMessage: String?

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

    var sendErrorIsPresented: Binding<Bool> {
        Binding(
            get: { self.sendErrorMessage != nil },
            set: { isPresented in
                if !isPresented {
                    self.sendErrorMessage = nil
                }
            }
        )
    }

    func refreshActivity() {
        items = ClipboardActivityBridge.recentItems()
            .compactMap(ClipboardStreamItem.init(activityItem:))
            .sorted { lhs, rhs in
                lhs.timestamp > rhs.timestamp
            }
    }

    func copy(_ item: ClipboardStreamItem) {
        guard let sourceItem = item.activitySourceItem else {
            return
        }

        do {
            try ClipboardActivityBridge.copy(sourceItem)
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

    func send() {
        guard sendState != .sending else {
            return
        }

        sendState = .sending
        do {
            _ = try OutgoingClipboardBridge.sendCurrentPasteboard()
            refreshActivity()
            sendState = .sent

            Task {
                try? await Task.sleep(nanoseconds: 1_400_000_000)
                await MainActor.run {
                    if self.sendState == .sent {
                        self.sendState = .idle
                    }
                }
            }
        } catch {
            sendState = .idle
            sendErrorMessage = error.localizedDescription
        }
    }
}

private enum NetworkIndicatorMode: Sendable {
    case ok
    case searching
    case needsSetup
}

private struct NetworkTrafficState {
    private static let significantByteDelta: UInt64 = 16 * 1024
    private static let activeHoldTime: TimeInterval = 1.2

    private var previousBytesSent: UInt64?
    private var previousBytesReceived: UInt64?
    // Public deadlines — the indicator compares these against its TimelineView's
    // own context.date each frame, so liveness no longer depends on the parent
    // view re-evaluating its body. Snapshot semantics: if the parent doesn't
    // re-render, these stay at their last-set values, and the indicator
    // correctly stops drawing once Date() crosses them.
    var sendingUntil = Date.distantPast
    var receivingUntil = Date.distantPast

    mutating func observe(bytesSent: UInt64, bytesReceived: UInt64, now: Date) {
        defer {
            previousBytesSent = bytesSent
            previousBytesReceived = bytesReceived
        }

        guard let previousBytesSent, let previousBytesReceived else {
            return
        }

        if bytesSent >= previousBytesSent,
           bytesSent - previousBytesSent >= Self.significantByteDelta {
            sendingUntil = now.addingTimeInterval(Self.activeHoldTime)
        }

        if bytesReceived >= previousBytesReceived,
           bytesReceived - previousBytesReceived >= Self.significantByteDelta {
            receivingUntil = now.addingTimeInterval(Self.activeHoldTime)
        }
    }
}

private struct NetworkToolbarIndicator: View {
    let mode: NetworkIndicatorMode
    // The traffic-active deadlines, pushed in from the parent's NetworkTrafficState.
    // We compare them against the outer TimelineView's context.date each tick,
    // independent of whether the parent re-evaluates its body — that's what makes
    // the indicator stop reliably when the deadline expires (and not get stuck
    // waiting for some unrelated state change to wake the view tree).
    let sendingUntil: Date
    let receivingUntil: Date

    // Animation periods (seconds, full cycle).
    private static let sweepPeriod: Double = 1.4   // radar sweep, linear rotation
    private static let badgePeriod: Double = 1.7   // setup-needed badge pulse (autoreverse 0.85s)
    private static let glyphPeriod: Double = 1.12  // send/receive arrow bob (autoreverse 0.56s)

    // Liveness-check cadence. Slow enough to be idle-cheap, fast enough that the
    // visual lag between "deadline expired" and "indicator goes dark" is well
    // under the 1.2 s hold time the deadlines build in anyway.
    private static let livenessTick: Double = 0.25

    var body: some View {
        // Outer schedule runs at a steady 4 Hz, recomputes which sub-views should
        // exist (based on `mode` and the absolute deadline Dates), and otherwise
        // does ~no work. The 60 fps animation cost is paid only by the inner
        // TimelineViews, which exist only while their conditional is true — i.e.,
        // not when the indicator is idle.
        TimelineView(.periodic(from: Date(), by: Self.livenessTick)) { outerContext in
            let now = outerContext.date
            let isSending = sendingUntil > now
            let isReceiving = receivingUntil > now

            ZStack {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .symbolRenderingMode(.hierarchical)

                if mode == .searching {
                    TimelineView(.animation) { animContext in
                        let t = animContext.date.timeIntervalSinceReferenceDate
                        let angle = t.truncatingRemainder(dividingBy: Self.sweepPeriod) / Self.sweepPeriod * 360.0
                        Circle()
                            .trim(from: 0.08, to: 0.28)
                            .stroke(
                                .secondary,
                                style: StrokeStyle(lineWidth: 1.5, lineCap: .round)
                            )
                            .frame(width: 24, height: 24)
                            .rotationEffect(.degrees(angle))
                    }
                    .transition(.opacity)
                }

                if mode == .needsSetup {
                    TimelineView(.animation) { animContext in
                        let t = animContext.date.timeIntervalSinceReferenceDate
                        let phase = (sin(t * 2.0 * .pi / Self.badgePeriod) + 1.0) / 2.0
                        Image(systemName: "exclamationmark.circle.fill")
                            .symbolRenderingMode(.palette)
                            .foregroundStyle(.white, .orange)
                            .font(.system(size: 12, weight: .semibold))
                            .background(
                                Circle()
                                    .fill(.background)
                                    .frame(width: 13, height: 13)
                            )
                            .scaleEffect(1.0 + phase * 0.18)
                            .offset(x: 9, y: -9)
                    }
                    .transition(.scale.combined(with: .opacity))
                }

                if mode != .needsSetup {
                    if isSending {
                        TimelineView(.animation) { animContext in
                            let t = animContext.date.timeIntervalSinceReferenceDate
                            let phase = (sin(t * 2.0 * .pi / Self.glyphPeriod) + 1.0) / 2.0
                            NetworkTrafficGlyph(
                                symbolName: "arrow.up.circle.fill",
                                color: .blue,
                                yOffset: -8 + CGFloat(phase) * -6,
                                opacity: 1.0 - phase * 0.44
                            )
                        }
                        .transition(.scale.combined(with: .opacity))
                    }

                    if isReceiving {
                        TimelineView(.animation) { animContext in
                            let t = animContext.date.timeIntervalSinceReferenceDate
                            let phase = (sin(t * 2.0 * .pi / Self.glyphPeriod) + 1.0) / 2.0
                            NetworkTrafficGlyph(
                                symbolName: "arrow.down.circle.fill",
                                color: .green,
                                yOffset: 8 + CGFloat(phase) * 6,
                                opacity: 1.0 - phase * 0.44
                            )
                        }
                        .transition(.scale.combined(with: .opacity))
                    }
                }
            }
            .frame(width: 28, height: 28)
        }
    }
}

private struct NetworkTrafficGlyph: View {
    let symbolName: String
    let color: Color
    let yOffset: CGFloat
    let opacity: Double

    var body: some View {
        Image(systemName: symbolName)
            .symbolRenderingMode(.palette)
            .foregroundStyle(.white, color)
            .font(.system(size: 11, weight: .bold))
            .background(
                Circle()
                    .fill(.background)
                    .frame(width: 11, height: 11)
            )
            .offset(x: 10, y: yOffset)
            .opacity(opacity)
    }
}

private struct SendBottomButton: View {
    let state: OutgoingSendState

    var body: some View {
        HStack(spacing: 8) {
            switch state {
            case .idle:
                Image(systemName: "paperplane")
                Text("Send")
            case .sending:
                ProgressView()
                    .tint(.white)
                Text("Sending")
            case .sent:
                Image(systemName: "checkmark")
                Text("Sent")
            }
        }
        .font(.callout.weight(.semibold))
        .foregroundStyle(.white)
        .padding(.horizontal, 18)
        .frame(height: 46)
        .background(
            Capsule(style: .continuous)
                .fill(Color.clippInk)
        )
        .overlay {
            Capsule(style: .continuous)
                .strokeBorder(.white.opacity(0.14))
        }
        .shadow(color: .black.opacity(0.18), radius: 14, y: 8)
    }
}

private enum ClipboardPayload {
    case text(String)
    case privateText(String)
    case privatePlaceholder
    case link(title: String, host: String, url: String)
    case image(Data)
}

private enum StreamDirection {
    case incoming
    case outgoing
}

private struct ClipboardStreamItem: Identifiable {
    let id: String
    let deviceName: String
    let timestamp: Date
    let direction: StreamDirection
    let payload: ClipboardPayload
    // True when the source app set an explicit OS-level privacy marker on this
    // clipboard item (as opposed to the receive-side heuristic catching it).
    let sourceMarked: Bool
    let activitySourceItem: ClipboardActivityItem?

    var isPrivatePlaceholder: Bool {
        if case .privatePlaceholder = payload {
            return true
        }
        return false
    }

    var time: String {
        timestamp.formatted(date: .omitted, time: .shortened)
    }

    var dayTitle: String {
        if Calendar.current.isDateInToday(timestamp) {
            return "Today"
        }

        return timestamp.formatted(date: .abbreviated, time: .omitted)
    }

    var alignment: HorizontalAlignment {
        direction == .outgoing ? .trailing : .leading
    }

    var frameAlignment: Alignment {
        direction == .outgoing ? .trailing : .leading
    }

    init?(activityItem: ClipboardActivityItem) {
        switch activityItem.kind {
        case .text:
            let text = activityItem.detailText.isEmpty ? activityItem.previewText : activityItem.detailText
            payload = .text(text)

        case .privateText:
            let text = activityItem.detailText.isEmpty ? activityItem.previewText : activityItem.detailText
            payload = .privateText(text)

        case .privatePlaceholder:
            payload = .privatePlaceholder

        case .link:
            let url = activityItem.detailText.isEmpty ? activityItem.previewText : activityItem.detailText
            let host = activityItem.linkHost.isEmpty ? activityItem.previewText : activityItem.linkHost
            payload = .link(title: host, host: host, url: url)

        case .image:
            guard let imageData = activityItem.imageData else {
                return nil
            }
            payload = .image(imageData)

        case .unsupported:
            return nil

        @unknown default:
            return nil
        }

        id = activityItem.identifier
        deviceName = activityItem.deviceName
        timestamp = activityItem.timestamp
        direction = activityItem.isOutgoing ? .outgoing : .incoming
        sourceMarked = activityItem.sourceMarked
        activitySourceItem = activityItem
    }
}

private struct EmptyClipboardActivityView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray")
                .font(.system(size: 34, weight: .regular))
                .foregroundStyle(.secondary)

            VStack(spacing: 4) {
                Text("No clipboard activity yet")
                    .font(.headline)

                Text("Recent clipboard items will appear here for reference.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .frame(maxWidth: 280)
    }
}

private struct ClipboardGroupView: View {
    let item: ClipboardStreamItem
    let isCopied: Bool
    let onInspect: () -> Void
    let onCopy: () -> Void

    var body: some View {
        HStack(alignment: .top) {
            if item.direction == .outgoing {
                Spacer(minLength: 54)
            }

            VStack(alignment: item.alignment, spacing: 7) {
                Text(item.deviceName)
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
            .frame(maxWidth: 330, alignment: item.frameAlignment)

            if item.direction == .incoming {
                Spacer(minLength: 54)
            }
        }
    }
}

private struct ClipboardBubble: View {
    let item: ClipboardStreamItem
    let isCopied: Bool
    let onInspect: () -> Void
    let onCopy: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Group {
                if item.isPrivatePlaceholder {
                    // Placeholder rows are informational; no tap-to-inspect,
                    // no copy-back. Nothing to inspect, nothing to copy.
                    ClipboardPayloadView(payload: item.payload,
                                         isOutgoing: item.direction == .outgoing,
                                         sourceMarked: item.sourceMarked)
                } else {
                    ClipboardPayloadView(payload: item.payload,
                                         isOutgoing: item.direction == .outgoing,
                                         sourceMarked: item.sourceMarked)
                        .contentShape(Rectangle())
                        .onTapGesture(perform: onInspect)
                }
            }

            HStack(spacing: 8) {
                Text(item.time)
                    .font(.caption2)
                    .foregroundStyle(item.direction == .outgoing ? .white.opacity(0.62) : .secondary)

                Spacer(minLength: 12)

                if item.direction == .incoming && !item.isPrivatePlaceholder {
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
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 12)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(item.direction == .outgoing ? Color.clippInk : Color(.secondarySystemGroupedBackground))
        )
        .overlay {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(item.direction == .outgoing ? .white.opacity(0.10) : .black.opacity(0.05))
        }
    }
}

private struct ClipboardPayloadView: View {
    let payload: ClipboardPayload
    let isOutgoing: Bool
    let sourceMarked: Bool

    var body: some View {
        switch payload {
        case .text(let text):
            Text(text)
                .font(.system(.callout, design: .monospaced))
                .foregroundStyle(isOutgoing ? .white : .primary)
                .lineSpacing(3)
                .lineLimit(8)
                .textSelection(.enabled)

        case .privateText(let text):
            PrivateLineView(text: text, isOutgoing: isOutgoing, sourceMarked: sourceMarked)

        case .privatePlaceholder:
            PrivatePlaceholderView(isOutgoing: isOutgoing)

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

        case .image(let data):
            ClipboardImagePreview(data: data)
        }
    }
}

private struct ClipboardImagePreview: View {
    let data: Data

    private static let thumbnailMaxPixelSize: CGFloat = 768

    var body: some View {
        if let thumb = Self.thumbnail(from: data, maxPixelSize: Self.thumbnailMaxPixelSize) {
            Image(uiImage: thumb)
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

    private static func thumbnail(from data: Data, maxPixelSize: CGFloat) -> UIImage? {
        let sourceOptions: [CFString: Any] = [
            kCGImageSourceShouldCache: false,
        ]
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions as CFDictionary) else {
            return nil
        }

        let options: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixelSize,
        ]

        guard let cg = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
            return nil
        }

        return UIImage(cgImage: cg)
    }
}

private struct PrivateLineView: View {
    let text: String
    let isOutgoing: Bool
    let sourceMarked: Bool

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

            if sourceMarked {
                Text(CLP_UI_PRIVATE_BADGE)
                    .font(.caption2)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .background(
                        Capsule().fill(Color.orange.opacity(0.18))
                    )
                    .foregroundStyle(isOutgoing ? .white : .primary)
            }
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

private struct PrivatePlaceholderView: View {
    let isOutgoing: Bool

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "lock.fill")
                .font(.callout.weight(.semibold))
                .foregroundStyle(isOutgoing ? .white.opacity(0.72) : .secondary)

            VStack(alignment: .leading, spacing: 2) {
                Text(CLP_UI_PRIVATE_PLACEHOLDER_TITLE)
                    .font(.callout.weight(.semibold))
                    .foregroundStyle(isOutgoing ? .white : .primary)

                Text(CLP_UI_PRIVATE_PLACEHOLDER_DETAIL)
                    .font(.caption)
                    .foregroundStyle(isOutgoing ? .white.opacity(0.72) : .secondary)
            }
        }
    }
}

private struct ClipboardInspectSheet: View {
    let item: ClipboardStreamItem
    let onCopy: () -> Void

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text(item.deviceName)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)

                    ClipboardInspectPayloadView(payload: item.payload, sourceMarked: item.sourceMarked)
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
                    if item.direction == .incoming {
                        Button(action: onCopy) {
                            Image(systemName: "doc.on.doc")
                        }
                        .accessibilityLabel("Copy")
                    }
                }
            }
        }
    }
}

private struct ClipboardInspectPayloadView: View {
    let payload: ClipboardPayload
    let sourceMarked: Bool

    var body: some View {
        switch payload {
        case .text(let text):
            Text(text)
                .font(.system(.body, design: .monospaced))
                .lineSpacing(4)
                .textSelection(.enabled)

        case .privateText(let text):
            PrivateLineView(text: text, isOutgoing: false, sourceMarked: sourceMarked)
                .padding(.vertical, 4)

        case .privatePlaceholder:
            PrivatePlaceholderView(isOutgoing: false)
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

private extension Color {
    static let clippInk = Color(red: 0.0, green: 15.0 / 255.0, blue: 54.0 / 255.0)
}

#Preview {
    ContentView()
}
