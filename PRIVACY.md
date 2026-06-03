# Privacy Policy

**TL;DR:** Clipp does not collect, transmit, or store any data about you on any server. Clipboard contents move directly between devices you've paired on your local network, encrypted end-to-end. Nothing is sent to the author or to any third party.

## What Clipp does not do

- No accounts, no sign-up, no login.
- No telemetry, no analytics, no crash reports sent off-device.
- No advertising identifiers (IDFA), no third-party SDKs that phone home.
- No servers operated by Clipp or its author.
- No use of cloud sync for clipboard contents or credentials.

## What stays on your device

- **Network credentials.** The group name and the cryptographic key derived from your shared passphrase are stored in your operating system's secure credential store — Keychain on iOS and macOS, Credential Manager / DPAPI on Windows. Credentials never leave your device unless you intentionally copy them to another of your own devices to pair it.
- **Preferences.** Your settings are stored in your operating system's preferences storage (`UserDefaults` on Apple platforms, the Windows registry on Windows). These contain feature toggles and your chosen group name.
- **Activity history.** Recent clipboard items received from peers are kept in memory only and discarded when Clipp quits. Clipp does not write clipboard content to disk.

## What goes over the network

- **Service discovery.** When running, Clipp advertises a Bonjour / DNS-SD service (`_clipp._tcp`) on your local network so trusted peers can find it. The advertisement includes a device name you choose and a public fingerprint derived from your shared secret; it does not include the secret itself.
- **Clipboard transfer.** When you copy on one device or share an item via the Share Extension, Clipp sends it directly to your other paired devices over the local network. Each transfer is encrypted using keys derived from your shared secret. No intermediate server is involved.
- **Nothing else.** Clipp does not make any other outbound connections. There is no remote configuration, no update check, and no background pings of any kind.

## Permissions Clipp asks for

- **Local Network access** Required to discover peers and transfer clipboard content over your Wi-Fi or wired LAN. Without it, Clipp cannot function.
- **No other permissions.** Clipp does not request access to your camera, microphone, photos, contacts, location, or any other sensitive resource.

## Third parties

- **App distribution.** Clipp is distributed through the Apple App Store (iOS) and GitHub Releases (macOS and Windows). Apple's and GitHub's respective privacy policies cover those channels.
- **Cryptography libraries.** Clipp links libsodium, xxHash, and Zstandard statically. These run entirely on your device and do not make network calls.

## Children

Clipp does not knowingly collect information from anyone, including children under 13. 

## Changes to this policy

Material changes will be reflected in this file in the [project repository](https://github.com/martona/clipp/blob/master/PRIVACY.md). The "Last updated" date below tracks the most recent revision.

## Contact

Open an issue at <https://github.com/martona/clipp/issues>.

---

**Last updated:** May 28, 2026
