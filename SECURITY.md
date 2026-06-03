# Security Policy

## Reporting a Vulnerability

If you discover a security issue in Clipp, please report it privately rather than opening a public GitHub issue. Preferred channel: **GitHub private vulnerability reporting** — use the "Report a vulnerability" button under the repo's [Security tab](https://github.com/martona/clipp/security/advisories/new).

Please include enough detail for the maintainer to reproduce the issue: affected version or commit, platform, steps, and any proof-of-concept code or packets.

## Scope

Clipp's threat model is documented in the [Security Model](docs/SECURITY-MODEL.md). In short, Clipp assumes:

- Devices configured with the same group name and passphrase are mutually trusted.
- The local or overlay network that Clipp runs over is itself trusted (LAN, Tailscale, etc.).
- The operating system, other installed software, and the user are not adversaries.

### In scope

Reports against the following are very welcome:

- Weaknesses in the discovery, handshake, or transport protocols that allow a device without the network secret to read clipboard data, produce valid traffic, or impersonate a peer.
- Memory safety bugs, crashes, or denial-of-service triggerable by an untrusted peer (or by an on-network attacker without the secret).
- Flaws in key derivation, fingerprint computation, or master-key handling.
- Logic flaws that allow a trusted peer to perform actions outside the documented behavior (for example, executing code on a peer rather than only transferring clipboard data).

### Out of scope

The following are not considered vulnerabilities, because they fall outside the assumed trust boundary:

- A trusted peer reading clipboard data — this is the intended behavior.
- Other software on the same machine (clipboard managers, malware, remote-desktop tools, the OS itself) reading the local clipboard.
- The user choosing a weak shared secret — Clipp does not attempt to enforce secret strength.
- The user installing Clipp on a device they do not actually trust.
- Other code running in the user's security context decrypting the network key — this is a property of the platform's user-scoped secret store (DPAPI on Windows, Keychain on macOS), not a Clipp implementation choice. There is no available primitive on either platform that restricts decrypt access to a specific binary against same-user attackers.

## Disclosure Process

Please give the maintainer a reasonable window to investigate and ship a fix before public disclosure. The maintainer will:

1. Acknowledge the report within 7 days.
2. Validate the issue and propose a remediation timeline.
3. Coordinate a disclosure window with the reporter, sized to the severity and complexity of the fix.
4. Credit the reporter in the release notes for the fix, unless he prefers to remain anonymous.

Coordinated disclosure timing is negotiable. If a fix is not feasible within the agreed window, the maintainer will discuss extension or partial disclosure with the reporter rather than letting the deadline pass silently.
