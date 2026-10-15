//
//  ClippApp.swift
//  Clipp
//
//  Created by Marton Anka on 5/23/26.
//

import AppIntents
import SwiftUI

@main
struct ClippApp: App {
    init() {
        ClippAppShortcuts.updateAppShortcutParameters()
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
