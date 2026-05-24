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
#define CLP_UI_ABOUT "About"
#define CLP_UI_OPEN_CLIPP "Open Clipp"
#define CLP_UI_ABOUT_CLIPP "About Clipp"
#define CLP_UI_EXIT_CLIPP "Exit Clipp"
#define CLP_UI_STATUS_TOOLTIP "Clipp Network Sync"

#define CLP_UI_NAME "Name"
#define CLP_UI_SECRET "Secret"
#define CLP_UI_NETWORK_KEY "Network Key"
#define CLP_UI_NETWORK_KEY_FINGERPRINT "Network key fingerprint. Used only on this screen; not in itself a secret."
#define CLP_UI_ENTER_NETWORK_SECRET "Enter network secret to create or join a network."
#define CLP_UI_SECRET_TOO_SHORT "Secret must be at least 8 characters."
#define CLP_UI_WORKING "... working ..."

#define CLP_UI_PEERS "Peers"
#define CLP_UI_NO_PEERS_HELP "Your devices will appear here when they're on the same network and running Clipp. Make sure they are are using the exact same network name and secret. Both are case-sensitive."
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
#define CLP_UI_UDP_PORT "UDP Port"
#define CLP_UI_LISTENER_IP "Listener IP"
#define CLP_UI_MULTICAST_IP "Multicast IP"
#define CLP_UI_HOST_ID "Host ID"
#define CLP_UI_CURRENT_HOST_ID "Current Host ID"
#define CLP_UI_RESET "Reset"
#define CLP_UI_HOST_ID_COLLISION_WARNING "Possible Host ID collision detected. If this device was restored from backup or cloned, reset Host ID."
#define CLP_UI_NETWORK_SETTINGS_APPLIED "Network settings applied."
#define CLP_UI_UNAVAILABLE "Unavailable"
#define CLP_UI_UNABLE_TO_RESET_HOST_ID "Unable to reset Host ID."
#define CLP_UI_HOST_ID_RESET "Host ID reset."

#define CLP_UI_LIVE_DIAGNOSTIC_OUTPUT "Live diagnostic output from Clipp."
#define CLP_UI_COPY_LOG_LINES_FORMAT "Copy %d lines"

#define CLP_UI_ABOUT_TITLE "Clipp v1.0"
#define CLP_UI_TAGLINE "Secure cross-platform clipboard sync for trusted devices."
#define CLP_UI_PROJECT "Project"
#define CLP_UI_COPYRIGHT "Copyright (C) 2026 Marton Anka"
#define CLP_UI_MIT_LICENSE "Released under the MIT License."
#define CLP_UI_REPOSITORY_LABEL "github.com/martona/clipp"
#define CLP_UI_REPOSITORY_URL "https://github.com/martona/clipp"
#define CLP_UI_OPEN_SOURCE_ACKNOWLEDGEMENTS "Open Source Acknowledgements"
#define CLP_UI_ACK_LIBSODIUM "libsodium - ISC-licensed cryptography library"
#define CLP_UI_ACK_LODEPNG "lodepng - zlib-licensed PNG encoder/decoder"
#define CLP_UI_ACK_XXHASH "xxHash - BSD-2-Clause non-cryptographic hashing"
#define CLP_UI_ACK_ZSTD "Zstandard (zstd) - BSD-licensed compression"
#define CLP_UI_THIRD_PARTY_LICENSE_NOTE "Third-party license terms remain with their respective projects."
