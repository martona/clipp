#pragma once

#define CLP_WIDEN2(value) L##value
#define CLP_WIDEN(value) CLP_WIDEN2(value)
#define CLP_W(value) CLP_WIDEN(value)

#ifdef __OBJC__
#define CLP_NS2(value) @value
#define CLP_NS(value) CLP_NS2(value)
#endif

#define CLP_UI_APP_NAME "Clipp"
#define CLP_UI_NETWORK "Network"
#define CLP_UI_SETTINGS "Settings"
#define CLP_UI_LOGS "Logs"
#define CLP_UI_DIAGNOSTICS "Diagnostics"
#define CLP_UI_ABOUT "About"
#define CLP_UI_OPEN_CLIPP "Open Clipp"
#define CLP_UI_ABOUT_CLIPP "About Clipp"
#define CLP_UI_EXIT_CLIPP "Exit Clipp"
#define CLP_UI_STATUS_TOOLTIP "Clipp"

#define CLP_UI_NAME "Group name"
#define CLP_UI_SECRET "Passphrase"
#define CLP_UI_NETWORK_KEY "Group key"
#define CLP_UI_NETWORK_KEY_FINGERPRINT "Group key fingerprint for confirmation. Not itself a secret."
#define CLP_UI_ENTER_NETWORK_SECRET "Enter a group name and passphrase to pair this device with your others."
#define CLP_UI_SECRET_TOO_SHORT "Passphrase must be at least 8 characters."
#define CLP_UI_WORKING "... working ..."

#define CLP_UI_CLIPBOARD "Clipboard"
#define CLP_UI_CLIPBOARD_EMPTY "Your clipboard syncs automatically. Recent text and images will appear here for reference."
#define CLP_UI_NO_NETWORK_KEY_CONFIGURED "Not paired yet"
#define CLP_UI_THIS_DEVICE "This device"
#define CLP_UI_COPY "Copy"
#define CLP_UI_LINK "Link"
#define CLP_UI_TEXT "Text"
#define CLP_UI_IMAGE "Image"
#define CLP_UI_PRIVATE_TEXT "Private text"
#define CLP_UI_PRIVATE_BADGE "private"
#define CLP_UI_PRIVATE_PLACEHOLDER_TITLE "Marked private"
#define CLP_UI_PRIVATE_PLACEHOLDER_DETAIL "The source app asked Clipp not to sync this clipboard item."
#define CLP_UI_UNSUPPORTED_CLIPBOARD_ITEM "Unsupported clipboard item"

// Command-line helper banner (macOS GUI). Shown once a network key exists and the
// `clipp` CLI is not yet on a common PATH location, nudging the user to symlink it.
// Dismissible permanently. The command itself is built at the use site; on MAS we
// cannot create the symlink for the user (sandbox), so we only show the command.
#define CLP_UI_CLI_BANNER_TITLE "Use Clipp from the command line"
#define CLP_UI_CLI_BANNER_BODY "Run this in Terminal to add the clipp command to your PATH, then use clipp copy and clipp paste in scripts and over SSH."
#define CLP_UI_CLI_BANNER_COPY "Copy command"
#define CLP_UI_CLI_BANNER_COPIED "Copied"
#define CLP_UI_CLI_BANNER_DISMISS "Dismiss"

#define CLP_UI_PEERS "Paired devices"
#define CLP_UI_NO_PEERS_HELP "Your other devices appear here once they're paired and on the same local network. Use the exact same group name and passphrase on each device - both are case-sensitive."
#define CLP_UI_UNKNOWN_HOST "(unknown host)"
#define CLP_UI_CONNECTED "Connected"
#define CLP_UI_NOT_CONNECTED "Not connected"
#define CLP_UI_CONNECTED_FOR "Connected for "
#define CLP_UI_DAY_SUFFIX " day, "
#define CLP_UI_DAYS_SUFFIX " days, "
#define CLP_UI_BYTES_SENT "Bytes sent:"
#define CLP_UI_BYTES_RECEIVED "Bytes received:"
#define CLP_UI_INCOMING "Incoming:"
#define CLP_UI_OUTGOING "Outgoing:"

#define CLP_UI_TCP_PORT "TCP Port"
#define CLP_UI_LISTENER_IP "Listener IP"
#define CLP_UI_APPLY_NETWORK_SETTINGS "Apply Network Settings"
#define CLP_UI_HOST_ID "Host ID"
#define CLP_UI_CURRENT_HOST_ID "Current Host ID"
#define CLP_UI_RESET "Reset"
#define CLP_UI_HOST_ID_COLLISION_WARNING "Possible Host ID collision detected. If this device was restored from backup or cloned, reset Host ID."
#define CLP_UI_NETWORK_SETTINGS_APPLIED "Network settings applied."
#define CLP_UI_UNAVAILABLE "Unavailable"
#define CLP_UI_UNABLE_TO_RESET_HOST_ID "Unable to reset Host ID."
#define CLP_UI_HOST_ID_RESET "Host ID reset."
#define CLP_UI_CLIPBOARD_HISTORY "Clipboard history"
#define CLP_UI_HISTORY_MEMORY_LIMIT "Memory"
#define CLP_UI_HISTORY_TIME_LIMIT "Time"
#define CLP_UI_HISTORY_ITEM_LIMIT "Items"
#define CLP_UI_UNLIMITED "Unlimited"
#define CLP_UI_CLIPBOARD_HISTORY_SETTINGS_APPLIED "Clipboard history settings applied."

#define CLP_UI_PRIVACY "Privacy"
#define CLP_UI_HONOR_PRIVACY_MARKERS "Honor 'don't sync' requests from other apps"
#define CLP_UI_HONOR_PRIVACY_MARKERS_HELP "When other apps mark clipboard items as private (passwords from Chrome, password managers, etc.), Clipp will not sync the content to your other devices."
#define CLP_UI_PRIVACY_SETTINGS_APPLIED "Privacy settings applied."

#define CLP_UI_LIVE_DIAGNOSTIC_OUTPUT "Live diagnostic output from Clipp."
#define CLP_UI_COPY_LOG_LINES_FORMAT "Copy %d lines"

// Just the product name; About screens compose "<title> v<version>" at the use
// site using the platform's version source (CLIPP_VERSION_STRING_3PART from
// version.h for Windows/macOS, Bundle.main for iOS).
#define CLP_UI_ABOUT_TITLE "Clipp"
#define CLP_UI_TAGLINE "Cross-platform clipboard sync for trusted devices."
#define CLP_UI_PROJECT "Project"
#define CLP_UI_COPYRIGHT "Copyright (C) 2026 Marton Anka"
#define CLP_UI_MIT_LICENSE "Released under the MIT License."
#define CLP_UI_REPOSITORY_LABEL "github.com/martona/clipp"
#define CLP_UI_REPOSITORY_URL "https://github.com/martona/clipp"
#define CLP_UI_OPEN_SOURCE_ACKNOWLEDGEMENTS "Open Source Acknowledgements"
#define CLP_UI_ACK_LIBSODIUM "libsodium - ISC-licensed cryptography library"
#define CLP_UI_ACK_XXHASH "xxHash - BSD-2-Clause non-cryptographic hashing"
#define CLP_UI_ACK_ZSTD "Zstandard (zstd) - BSD-licensed compression"
#define CLP_UI_THIRD_PARTY_LICENSE_NOTE "Third-party license terms remain with their respective projects."
