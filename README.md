# Clipp

[![Windows CI](https://github.com/martona/clipp/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/martona/clipp/actions/workflows/windows-ci.yml)
[![macOS CI](https://github.com/martona/clipp/actions/workflows/macos-ci.yml/badge.svg)](https://github.com/martona/clipp/actions/workflows/macos-ci.yml)
[![iOS CI](https://github.com/martona/clipp/actions/workflows/ios-ci.yml/badge.svg)](https://github.com/martona/clipp/actions/workflows/ios-ci.yml)

Secure cross-platform clipboard sync for trusted devices.

Clipp is a free, open source, peer-to-peer clipboard sync utility for Windows, macOS, and iOS. It is built for the boring case: devices you already trust, on a network you already control, sharing clipboard text and images without routing your clipboard through someone else's cloud.

I wrote Clipp because I needed this exact thing, and the usual options kept failing one or more basic tests: not open source, cloud-dependent, not free, or folded into a larger kitchen-sink app whose job was no longer just clipboard sync. Clipp tries to stay narrow: discover nearby peers, verify device trust, move clipboard data directly, and otherwise stay out of the way.

Clipp is LAN-first by design. If you want the same workflow across networks, use an overlay such as Tailscale or NostrVPN and let Clipp keep doing the simple peer-to-peer part.

## Screenshots

<table>
  <tr>
    <td align="center" valign="top">
      <img src="docs/screenshots/windows.png" alt="Clipp on Windows: Network tab with key fingerprint and peer list" width="400"><br>
      <em>Windows — peer discovery, with key fingerprint and connected devices</em>
    </td>
    <td align="center" valign="top">
      <img src="docs/screenshots/macos.png" alt="Clipp on macOS: clipboard activity stream with text, masked password, and an image received from iPhone" width="400"><br>
      <em>macOS — clipboard activity stream: text, masked password, image from iPhone</em>
    </td>
  </tr>
</table>

## What It Does

Clipp moves clipboard history between your own devices without a cloud service in the middle.

- Syncs clipboard text and images across Windows, macOS, and iOS.
- Discovers peers on the local network automatically.
- Sends clipboard data directly between devices.
- Shows recent clipboard activity so you can copy an earlier item again.
- Works over trusted VPN or mesh networks when you want the same setup away from your LAN.

## Security Model

Clipp is designed for a specific trust model: your own devices, on a network or VPN you already trust. It is not meant to be an open pairing protocol for strangers on the same Wi-Fi, and it is not a cloud account system with remote device management.

Devices join a Clipp network by using the same network name and secret. Clipp derives a master key from that input, stores it in platform-protected storage, and shows a fingerprint so you can verify that devices are configured with the same key. The fingerprint is not a secret; it is just a way to detect mismatched setup. Discovery, handshakes, fingerprints, and encrypted streams use separate keys derived from that master key.

Local discovery is encrypted and authenticated. Devices that do not know the key should not be able to read or produce valid discovery packets; they should just see opaque multicast traffic.

Clipboard transfers are sent directly between peers over TCP. The connection handshake is authenticated with keys derived from the master key, then each connection uses fresh ephemeral Diffie-Hellman session keys for encrypted clipboard messages. Clipp uses libsodium primitives for this rather than trying to invent its own cryptography.

Clipboard data is still clipboard data. If a trusted device receives it, that device can read it. If malware, remote desktop software, another clipboard manager, or the operating system can read your local clipboard, Clipp cannot prevent that. Clipp also does not protect you from choosing a weak shared secret, sharing the secret with the wrong person, or running it on a device you do not actually trust.

On-device state is local to the device. The master key is stored through the OS key store. Recent clipboard activity is retained in memory only and not persisted to disk. Treat every configured device as part of the same trust boundary.

A note on passwords: single-line text that does not contain whitespace is assumed to be a password and is masked in the activity stream. This gives you safety from prying eyes, but is not a serious security boundary.

Clipp is LAN-first. If you want to use it across networks, put the devices on a trusted VPN or mesh network and keep the Clipp listener off the public internet. The master key is still required, but the intended outer boundary is a private network you control.

To report a vulnerability, see [SECURITY.md](SECURITY.md).

## Platform Status

<!-- TODO: user-facing platform support table. "Will this work on my device?"
     Suggested shape: per-OS minimum version, supported architectures, and any
     known caveats per platform (e.g. iOS device install requires Xcode for now). -->

## Installation

<!-- TODO: per-platform install instructions for end users.
     Suggested shape:
       - Releases pointer (GitHub Releases link once builds are published)
       - Windows: download, unzip, run
       - macOS: download, drag to Applications, allow on first run
       - iOS: TestFlight or sideload via Xcode
     Until releases are published, point readers at "Building From Source". -->

Prebuilt binaries are not yet published. See [Building From Source](#building-from-source) below.

## Usage

<!-- TODO: end-user usage walkthrough.
     Suggested shape:
       - First run: pick a network name and secret
       - Verify the fingerprint matches on a second device
       - How clipboard sync triggers (copy on A, paste on B)
       - The activity stream: pinning, re-copying, image preview
       - Tray / menu bar behavior, quitting vs minimizing -->

## Troubleshooting

<!-- TODO: runtime troubleshooting (not build troubleshooting — that lives in BUILDING.md).
     Suggested topics:
       - Devices don't see each other (multicast blocked / VLAN isolation)
       - Fingerprints don't match between devices
       - Clipboard sync works one direction but not the other
       - Windows: app doesn't appear in the tray
       - macOS: app gets killed on sleep / Gatekeeper warning on first run
       - iOS: simulator can't discover the Mac it's running on -->

## FAQ

**Will Clipp ever support Linux or Android?**

<!-- TODO: answer. -->

**Can I sync over the internet without a VPN?**

<!-- TODO: answer. The intro implies "no — use a trusted overlay". Confirm and expand. -->

**Does Clipp upload anything to a server?**

<!-- TODO: answer. Privacy posture. -->

**I forgot the network secret. Can I recover it?**

<!-- TODO: answer. -->

## Building From Source

See [BUILDING.md](BUILDING.md) for prerequisites and build instructions for Windows, macOS, and iOS.

## Project Status

<!-- TODO: describe the current maturity (e.g. early-stage / public preview /
     stable), maintenance posture (actively developed / steady state), and
     breaking-change expectations for the wire protocol or on-disk state. -->

## Contributing

<!-- TODO: contribution guidelines.
     Suggested shape:
       - How to file issues
       - PR expectations (scope, tests, CI must pass)
       - Code style pointers (or "match surrounding code")
       - Pointer to BUILDING.md for setup -->

## License

Clipp is released under the MIT License. See [LICENSE.md](LICENSE.md).
