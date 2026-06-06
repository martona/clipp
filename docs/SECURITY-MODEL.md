# Security Model

Clipp is designed for a specific trust model: your own devices, on a local network or VPN. It is not meant to be an open pairing protocol for strangers on the same Wi-Fi, and it is not a cloud account system with remote device management.

## Joining a group

Devices join a Clipp group by using the same group name and passphrase. Clipp derives a master key from that input, stores it in platform-protected storage where available (keychain on macOS/iOS, DPAPI on Windows), and shows a fingerprint so you can verify that devices are configured with the same key. The fingerprint is not a secret; it is just a way to detect mismatched setup. Discovery, handshakes, fingerprints, and encrypted streams use separate keys derived from that master key.

## Discovery

Local discovery is encrypted and authenticated. Devices that do not know the key should not be able to read or produce valid discovery packets; they should just see opaque multicast traffic.

## Transport

Clipboard transfers are sent directly between peers over TCP. The connection handshake is authenticated with keys derived from the master key, then each connection uses fresh ephemeral Diffie-Hellman session keys. Clipp uses libsodium primitives for this rather than trying to invent its own cryptography.

## What Clipp does not protect against

Clipboard data is still clipboard data. If a trusted device receives it, that device can read it. If malware, remote desktop software, another clipboard manager, or the operating system can read your local clipboard, Clipp cannot prevent that. Clipp also does not protect you from choosing a weak shared secret, sharing the secret with the wrong person, or running it on a device you do not actually trust.

## On-device state

On-device state is local to the device. The master key is stored through the OS key store. Recent clipboard activity is retained in memory only and not persisted to disk. Treat every configured device as part of the same trust boundary.

## Passwords on the clipboard

Single-line text that does not contain whitespace is assumed to be a password and is masked in the activity stream. This gives you safety from prying eyes, but is not a serious security boundary. Chrome and various password managers set a clipboard marker asking other apps to exclude the item from any kind of clipboard history — local managers and cloud sync alike. Clipp honors that marker and will not sync such items; a placeholder PRIVATE TEXT will appear in the activity stream. You can override this behavior on the settings screen however. All such items will sync from thereon but will always be masked, regardless of heuristics.

## Network boundary

Clipp is LAN-only. If you want to use it across networks, put the devices on a VPN and keep the Clipp listener off the public internet. The master key is still required, but the intended outer boundary is a private network you control.

## Reporting a vulnerability

To report a security issue, see [SECURITY.md](../SECURITY.md).
