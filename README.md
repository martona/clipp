# Clipp

Secure cross-platform clipboard sync for trusted devices.

Clipp is a free, open source, peer-to-peer clipboard sync utility for Windows, macOS, and iOS. It is built for the boring case: devices you already trust, on a network you already control, sharing clipboard text and images without routing your clipboard through someone else's cloud.

I wrote Clipp because I needed this exact thing, and the usual options kept failing one or more basic tests: not open source, cloud-dependent, not free, or folded into a larger kitchen-sink app whose job was no longer just clipboard sync. Clipp tries to stay small and explicit: discover nearby peers, verify device trust, move clipboard data directly, and otherwise stay out of the way.

Clipp is LAN-first by design. If you want the same workflow across networks, use a trusted overlay such as Tailscale or NostrVPN and let Clipp keep doing the simple peer-to-peer part.

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

If you find a security issue, please report it privately to the project maintainer before publishing details.

## Platform Status

TODO.

## Installation

TODO.

## Usage

TODO.

## Building From Source

See [BUILDING.md](BUILDING.md) for prerequisites and build instructions for Windows, macOS, and iOS.

## Troubleshooting

TODO.

## Project Status

TODO.

## Contributing

TODO.
