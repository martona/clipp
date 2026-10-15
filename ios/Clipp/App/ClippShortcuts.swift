// Shared by the iOS app's synchronized source group and the macOS CMake target.
import AppIntents
import Foundation

struct CopyLatestClipboardItemIntent: AppIntent {
    static var title: LocalizedStringResource = "Copy Latest Clipboard Item"
    static var description = IntentDescription(
        "Copy the newest item in Clipp's local history to the clipboard and share it with connected devices. Opens Clipp to access the clipboard."
    )

    // Keep the action in the app process, where the live history and clipboard
    // bridges exist. Also supports our iOS 16 / macOS 14 deployment targets.
    static var openAppWhenRun: Bool = true

    @MainActor
    func perform() async throws -> some IntentResult {
        try AppleShortcutsBridge.copyLatestItem()
        return .result()
    }
}

struct ClippAppShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: CopyLatestClipboardItemIntent(),
            phrases: [
                "Copy the latest clipboard item in \(.applicationName)",
                "Copy latest from \(.applicationName)"
            ],
            shortTitle: "Copy Latest Item",
            systemImageName: "doc.on.clipboard"
        )
    }
}

#if os(macOS)
@_cdecl("CLPRegisterAppleShortcuts")
func registerAppleShortcuts() {
    ClippAppShortcuts.updateAppShortcutParameters()
}
#endif
