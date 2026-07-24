//
//  RegistersView.swift
//  Clipp
//
//  The "Registers" segment: the named-register CRDT surfaced on iOS. Registers
//  are saved clipboard items you name and keep; they sync across the mesh exactly
//  like the desktop popup's register column. This view lists them, and offers
//  Copy (make-current), Save (promote a clipboard item), Rename, Mark private,
//  Delete, and a one-deep Undo — all through CLPRegisterBridge.
//

import SwiftUI
import Combine
import UIKit
import ImageIO

// MARK: - Model

// Immutable snapshot of one register for the list. Mirrors CLPRegisterItem; the
// bridge holds the real value even for private records, and this view is what
// masks them (PrivateLineView) — same contract as the desktop popup.
struct RegisterRowItem: Identifiable {
    let id: String          // the name; register names are unique
    let name: String
    let previewText: String
    let fullText: String?   // nil for a binary (image) register
    let isPrivate: Bool
    let isBinary: Bool
    let imageData: Data?
    let valueSize: UInt64
    let touched: Date

    init(_ item: RegisterItem) {
        id = item.name
        name = item.name
        previewText = item.previewText
        fullText = item.fullText
        isPrivate = item.isPrivate
        isBinary = item.isBinary
        imageData = item.imageData
        valueSize = item.valueSize
        touched = item.touched
    }
}

@MainActor
final class RegistersViewModel: ObservableObject {
    @Published var registers: [RegisterRowItem] = []
    @Published var errorMessage: String?
    // Name of the register a make-current ("Copy") just acted on, for a brief ✓.
    @Published var copiedName: String?
    // Name to scroll to + flash after an undo restores it.
    @Published var revealName: String?
    // Non-nil when the one-deep undo slot holds a register delete we can reverse.
    @Published var undoName: String?

    var errorIsPresented: Binding<Bool> {
        Binding(
            get: { self.errorMessage != nil },
            set: { if !$0 { self.errorMessage = nil } }
        )
    }

    func refresh() {
        registers = RegisterBridge.registers().map(RegisterRowItem.init)
        refreshUndo()
    }

    // Keep the published undo state in lockstep with the bridge's single slot.
    private func refreshUndo() {
        undoName = RegisterBridge.pendingUndoKind() == .register
            ? RegisterBridge.pendingUndoRegisterName()
            : nil
    }

    func makeCurrent(_ item: RegisterRowItem) {
        do {
            try RegisterBridge.makeCurrent(name: item.name)
            copiedName = item.name
            refreshUndo()
            Task {
                try? await Task.sleep(nanoseconds: 1_400_000_000)
                if self.copiedName == item.name { self.copiedName = nil }
            }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func delete(_ item: RegisterRowItem) {
        do {
            try RegisterBridge.delete(name: item.name)
            refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func rename(_ item: RegisterRowItem, to newName: String) {
        let trimmed = newName.trimmingCharacters(in: .whitespacesAndNewlines)
            .precomposedStringWithCanonicalMapping   // NFC, like the desktop editor
        guard trimmed != item.name else { return }
        guard RegisterBridge.isValidName(trimmed) else {
            errorMessage = "That name isn't valid. Use up to 64 characters, and avoid slashes, ?, and *."
            return
        }
        do {
            try RegisterBridge.rename(from: item.name, to: trimmed)
            refresh()
            revealName = trimmed
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func setPrivate(_ item: RegisterRowItem, _ isPrivate: Bool) {
        do {
            try RegisterBridge.setPrivate(name: item.name, private: isPrivate)
            refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    // Promote a clipboard activity item to a named register ("Save").
    func save(activityItemID: UInt64, name: String, markPrivate: Bool) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
            .precomposedStringWithCanonicalMapping
        guard RegisterBridge.isValidName(trimmed) else {
            errorMessage = "That name isn't valid. Use up to 64 characters, and avoid slashes, ?, and *."
            return
        }
        do {
            try RegisterBridge.save(activityItemID: activityItemID, asName: trimmed, markPrivate: markPrivate)
            refresh()
            revealName = trimmed
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func undo() {
        // Capture before the slot is consumed so we can reveal the restored row.
        let restored = RegisterBridge.pendingUndoRegisterName()
        do {
            try RegisterBridge.undoDelete()
            refresh()
            revealName = restored
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

// MARK: - List

struct RegistersView: View {
    @ObservedObject var model: RegistersViewModel

    @State private var inspected: RegisterRowItem?
    @State private var renameTarget: RegisterRowItem?
    @State private var renameText = ""

    var body: some View {
        Group {
            if model.registers.isEmpty {
                EmptyRegistersView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color(.systemGroupedBackground))
            } else {
                ScrollViewReader { proxy in
                    List {
                        ForEach(model.registers) { item in
                            RegisterRowView(item: item, isCopied: model.copiedName == item.name)
                                .id(item.id)
                                .listRowBackground(
                                    model.revealName == item.name
                                        ? Color.accentColor.opacity(0.16)
                                        : Color(.secondarySystemGroupedBackground)
                                )
                                .contentShape(Rectangle())
                                .onTapGesture { inspected = item }
                                .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                                    Button(role: .destructive) {
                                        model.delete(item)
                                    } label: {
                                        Label("Delete", systemImage: "trash")
                                    }
                                }
                                .contextMenu {
                                    RegisterActionButtons(
                                        item: item,
                                        model: model,
                                        onRename: { beginRename(item) }
                                    )
                                }
                        }
                    }
                    .listStyle(.insetGrouped)
                    .animation(.default, value: model.registers.map(\.id))
                    .onChange(of: model.revealName) { name in
                        guard let name else { return }
                        withAnimation { proxy.scrollTo(name, anchor: .center) }
                        Task {
                            try? await Task.sleep(nanoseconds: 1_300_000_000)
                            if model.revealName == name { model.revealName = nil }
                        }
                    }
                }
            }
        }
        .sheet(item: $inspected) { item in
            RegisterInspectSheet(item: item, model: model)
        }
        .alert("Rename Register", isPresented: renameIsPresented) {
            TextField("Register name", text: $renameText)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) { renameTarget = nil }
            Button("Rename") {
                if let target = renameTarget {
                    model.rename(target, to: renameText)
                }
                renameTarget = nil
            }
        } message: {
            Text("Choose a new name for this register.")
        }
        .alert("Couldn't Update Register", isPresented: model.errorIsPresented) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.errorMessage ?? "Something went wrong.")
        }
    }

    private var renameIsPresented: Binding<Bool> {
        Binding(
            get: { renameTarget != nil },
            set: { if !$0 { renameTarget = nil } }
        )
    }

    private func beginRename(_ item: RegisterRowItem) {
        renameText = item.name
        renameTarget = item
    }
}

// The register actions shared by the row context menu and the detail sheet.
// Rename is routed out via a callback because it needs a text-entry alert owned
// by the presenting container.
private struct RegisterActionButtons: View {
    let item: RegisterRowItem
    @ObservedObject var model: RegistersViewModel
    let onRename: () -> Void
    // Called after a mutating action. The detail sheet passes dismiss() (it holds
    // an immutable snapshot, so it must close once the item changes); the list
    // context menu passes nothing (the list refreshes in place).
    var afterAction: (() -> Void)? = nil

    var body: some View {
        Button {
            model.makeCurrent(item)
            afterAction?()
        } label: {
            Label("Copy", systemImage: "doc.on.doc")
        }

        Button(action: onRename) {
            Label("Rename", systemImage: "pencil")
        }

        Button {
            model.setPrivate(item, !item.isPrivate)
            afterAction?()
        } label: {
            Label(item.isPrivate ? "Mark Non-Private" : "Mark Private",
                  systemImage: item.isPrivate ? "lock.open" : "lock")
        }

        Button(role: .destructive) {
            model.delete(item)
            afterAction?()
        } label: {
            Label("Delete", systemImage: "trash")
        }
    }
}

// MARK: - Row

private struct RegisterRowView: View {
    let item: RegisterRowItem
    let isCopied: Bool

    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(item.name)
                    .font(.headline)
                    .lineLimit(1)
                    .truncationMode(.middle)

                valueLine

                HStack(spacing: 6) {
                    Text(RegisterFormat.relativeAge(item.touched))
                    if item.isPrivate {
                        Text(CLP_UI_PRIVATE_BADGE)
                            .padding(.horizontal, 6)
                            .padding(.vertical, 1)
                            .background(Capsule().fill(Color.orange.opacity(0.18)))
                    }
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }

            Spacer(minLength: 8)

            if isCopied {
                Image(systemName: "checkmark")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.green)
                    .transition(.opacity)
            }
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private var valueLine: some View {
        if item.isBinary {
            Label("Image", systemImage: "photo")
                .font(.footnote)
                .foregroundStyle(.secondary)
        } else if item.isPrivate {
            PrivateLineView(text: item.fullText ?? "", isOutgoing: false, sourceMarked: false)
        } else {
            Text(item.previewText)
                .font(.system(.footnote, design: .monospaced))
                .foregroundStyle(.secondary)
                .lineLimit(2)
        }
    }
}

// MARK: - Detail sheet

private struct RegisterInspectSheet: View {
    let item: RegisterRowItem
    @ObservedObject var model: RegistersViewModel

    @Environment(\.dismiss) private var dismiss
    @State private var renaming = false
    @State private var renameText = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    HStack(spacing: 8) {
                        Text(RegisterFormat.relativeAge(item.touched))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        if item.isPrivate {
                            Text(CLP_UI_PRIVATE_BADGE)
                                .font(.caption2)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 1)
                                .background(Capsule().fill(Color.orange.opacity(0.18)))
                        }
                    }

                    RegisterValueView(item: item)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(16)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle(item.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Done") { dismiss() }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        RegisterActionButtons(
                            item: item,
                            model: model,
                            onRename: {
                                renameText = item.name
                                renaming = true
                            },
                            afterAction: { dismiss() }
                        )
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                }
            }
            .safeAreaInset(edge: .bottom) {
                Button {
                    model.makeCurrent(item)
                    dismiss()
                } label: {
                    Label("Copy", systemImage: "doc.on.doc")
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity)
                        .frame(height: 48)
                        .background(Capsule(style: .continuous).fill(Color.clippInkRegisters))
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 12)
            }
            .alert("Rename Register", isPresented: $renaming) {
                TextField("Register name", text: $renameText)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Cancel", role: .cancel) {}
                Button("Rename") {
                    model.rename(item, to: renameText)
                    dismiss()
                }
            } message: {
                Text("Choose a new name for this register.")
            }
        }
        .presentationDetents([.medium, .large])
    }
}

private struct RegisterValueView: View {
    let item: RegisterRowItem

    var body: some View {
        if item.isBinary {
            if let data = item.imageData, let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            } else {
                Label("Image preview unavailable", systemImage: "photo")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        } else if item.isPrivate {
            PrivateLineView(text: item.fullText ?? "", isOutgoing: false, sourceMarked: item.isPrivate)
                .padding(.vertical, 4)
        } else {
            Text(item.fullText ?? "")
                .font(.system(.body, design: .monospaced))
                .lineSpacing(4)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

// MARK: - Undo bar

// Persists while the one-deep undo slot holds a register delete (mirrors the
// desktop popup's toolbar Undo button — NOT a timed toast). Any other register
// mutation disarms the slot and this disappears.
struct UndoRegisterBar: View {
    let name: String
    let onUndo: () -> Void

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "trash")
                .foregroundStyle(.secondary)

            Text("Deleted \u{201C}\(name)\u{201D}")
                .font(.subheadline)
                .lineLimit(1)
                .truncationMode(.middle)

            Spacer(minLength: 8)

            Button("Undo", action: onUndo)
                .font(.subheadline.weight(.semibold))
                .buttonStyle(.borderless)
        }
        .padding(.horizontal, 18)
        .frame(height: 50)
        .background(
            Capsule(style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))
        )
        .overlay(
            Capsule(style: .continuous)
                .strokeBorder(.black.opacity(0.06))
        )
        .shadow(color: .black.opacity(0.18), radius: 14, y: 8)
    }
}

// MARK: - Empty state

private struct EmptyRegistersView: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "tray.full")
                .font(.system(size: 34, weight: .regular))
                .foregroundStyle(.secondary)

            VStack(spacing: 4) {
                Text("No registers yet")
                    .font(.headline)

                Text("Registers are clipboard items you name and keep. Open a clipboard item and choose Save to Register — it syncs to your other devices.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .frame(maxWidth: 300)
        .padding()
    }
}

// MARK: - Formatting

enum RegisterFormat {
    private static let relativeFormatter: RelativeDateTimeFormatter = {
        let f = RelativeDateTimeFormatter()
        f.unitsStyle = .abbreviated
        return f
    }()

    static func relativeAge(_ date: Date) -> String {
        relativeFormatter.localizedString(for: date, relativeTo: Date())
    }
}

extension Color {
    // Same ink as the clipboard Send capsule (that one is file-private in
    // ContentView, so the registers UI keeps its own copy of the constant).
    static let clippInkRegisters = Color(red: 0.0, green: 15.0 / 255.0, blue: 54.0 / 255.0)
}
