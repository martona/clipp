# Packaging

Manifests for distributing Clipp via package managers. Nothing here rebuilds anything —
they point package managers at artifacts already published on GitHub Releases.

- **Linux** — [`nfpm.yaml`](nfpm.yaml) drives the `.deb` / `.rpm` / Arch `.pkg.tar.zst`
  packages built in the release workflow (one static-linked binary; only runtime dep is
  Avahi).
- **winget** — three manifests in [`winget/`](winget/); details below.

---

## winget — [`winget/`](winget/)

Three manifests (`version`, `installer`, `locale`) for `martona.clipp`, packaging the
**signed per-arch MSIX** published on GitHub Releases (`clipp-windows-{amd64,arm64}.msix`).

The MSIX registers the tray GUI (`clippmain.exe`) and a `clipp.exe` execution alias, so a
single `winget install martona.clipp` puts both the GUI and the `clipp copy/paste/ls` CLI
on PATH. MSIX uninstall is clean (no leftover command alias or files).

> Keep `winget/` manifests-only — `winget validate` and `wingetcreate` parse every file in
> the folder, so a stray README/non-manifest there fails them. That's why this doc lives
> one level up.

### First submission (creates the package)

Submit the seed manifests once, then a moderator merges:

- manual PR placing them at `manifests/m/martona/clipp/<version>/` in
  [`microsoft/winget-pkgs`](https://github.com/microsoft/winget-pkgs), or
- `wingetcreate submit --token <PAT> packaging/winget/`.

Automated validation runs (schema, hash match, signature + URL reachability, a sandbox
install test, SmartScreen). The Trusted-Signing signature clears the reputation checks —
it chains to a Microsoft-trusted root, so no manual cert trust is needed.

### Ongoing auto-bump (not wired yet)

Model it on yo's `.github/workflows/winget.yml`: the `vedantmgoyal9/winget-releaser`
action runs `wingetcreate update`, which **preserves this MSIX shape** and only swaps
version + URLs + `InstallerSha256` + `SignatureSha256` + `PackageFamilyName` on each
release. So the seed here defines the shape once.

- `identifier: martona.clipp`
- `installers-regex: '^clipp-windows-(amd64|arm64)\.msix$'`
- Repo secret `WINGET_TOKEN`: a classic PAT with `public_repo` scope on an account that
  forks `microsoft/winget-pkgs`. Until it's set the workflow should no-op cleanly.
- Verify the `winget-releaser` action's current owner/version/inputs before relying on it
  — the project has been renamed/transferred over time.

### Regenerating hashes by hand

`InstallerSha256` = SHA256 of each `.msix`. `SignatureSha256` = SHA256 of the
`AppxSignature.p7x` entry inside it. `PackageFamilyName` = `<Identity.Name>_<publisherHash>`,
where the hash is base32 of the first 8 bytes of SHA256(UTF-16LE publisher string).
`wingetcreate` computes all three automatically — prefer it over doing this by hand.
