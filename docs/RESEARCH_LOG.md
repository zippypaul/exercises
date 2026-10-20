# Slow Mail - research log (reMarkable Paper Pro)

All entries retrieved **2026-10-20** unless stated. Status: **VERIFIED** (I read the primary text), **SNIPPET** (search-result summary only), **UNVERIFIED** (memory/inference). `[vendor]` = reMarkable artifact, `[upstream]` = project repo/issue, `[3P]` = third-party doc.
Local copies of fetched files lived in the session scratchpad only (not committed).

## Access constraints observed

| Target | Result |
|---|---|
| developer.remarkable.com, remarkable.guide, support.remarkable.com, news.remarkable.com (WebFetch) | `getaddrinfo ENOTFOUND` |
| developer.remarkable.com, web.archive.org, archive.org, r.jina.ai (curl via proxy) | `CONNECT tunnel failed, response 403` |
| web.archive.org, archive.org via WebFetch | "unable to fetch" / ENOTFOUND |
| github.com (WebFetch) | works; curl to github.com atom feeds and `api.github.com` repos: 403 / "not enabled" |
| raw.githubusercontent.com, storage.googleapis.com (curl) | works |
| GitHub MCP tools for repos other than the session repo | access denied; I did not add repos (not requested) |
| medium.com, techwalla.com, davisr.me (WebFetch) | ENOTFOUND |

## Entries

### L1 - reMarkable/linux-imx-rm README
- URL: https://raw.githubusercontent.com/reMarkable/linux-imx-rm/HEAD/README.md (also github.com page via WebFetch). Status: **VERIFIED [vendor]**.
- Paper Pro = `ferrari` (`ferrari_defconfig`, `ferrari.its`, kernel branch `rmpp_6.1.55_v3.14.x`, 3.14.1.10). Paper Pro Move = `chiappa` (SDK `5.2.96`, OS dir `3.22.0.65`, env `/opt/codex/chiappa/5.2.96/environment-setup-cortexa55-remarkable-linux`, `mkimage_imx8 -soc IMX9`). Paper Pure = `tatsu` (SDK `5.7.119`, OS dir `3.27.0.97`, `/opt/codex/tatsu/5.7.119/environment-setup-cortexa55-remarkable-linux`).
- Installer URL pattern: `https://storage.googleapis.com/remarkable-codex-toolchain/<OS ver>/<codename>/remarkable-production-image-<sdk>-<codename>-public-x86_64-toolchain.sh`.
- Deploy shows `ssh root@10.11.99.1 mount -o remount,rw /` (rootfs read-only by default).

### L2 - SDK installer header, ferrari 5.7.119 (x86_64)
- URL: https://storage.googleapis.com/remarkable-codex-toolchain/3.27.0.97/ferrari/remarkable-production-image-5.7.119-ferrari-public-x86_64-toolchain.sh. HTTP HEAD 200, 482,953,304 bytes, last-modified 2026-05-21. First 30 KB read with a range request. Status: **VERIFIED [vendor]**.
- `DEFAULT_INSTALL_DIR="/opt/codex/ferrari/5.7.119"`; checks for `$target_sdk_dir/environment-setup-cortexa53-crypto-remarkable-linux`; requires xz and an x86_64 host; payload is a tar.xz after a `MARKER:` line.

### L3 - SDK 5.7.119 payload (streamed, listed, selected files extracted)
- Same URL as L2. Status: **VERIFIED [vendor]**.
- Env script exports `CC=aarch64-remarkable-linux-gcc -mcpu=cortex-a53+crc+crypto -mbranch-protection=standard --sysroot=...`, `OECORE_SDK_VERSION=5.7.119`, `CMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake`, `ARCH=arm64`, `CROSS_COMPILE=aarch64-remarkable-linux-`; refuses to run if `LD_LIBRARY_PATH` set. `OEToolchainConfig.cmake.d/OEQt6Toolchain.cmake` sets `QT_HOST_PATH`. `buildinfo`: DISTRO codex 5.7.119 (Yocto layers incl. meta-imx-bsp, meta-swupdate, meta-flutter).
- Target sysroot `sysroots/cortexa53-crypto-remarkable-linux`: `libQt6Core.so.6.8.2` and many other Qt 6.8.2 libs incl. Quick, QuickControls2, QuickTemplates2, QuickLayouts, QuickShapes, Svg, Network, DBus, WebSockets. No Widgets/Sql/Multimedia. Platform plugins: `libqminimal`, `libqoffscreen`, `libqvnc` only. `plugins/scenegraph/libqsgepaper.so` present. Generic plugins: evdev keyboard/mouse/tablet/touch. Also libssl.so.3, libcurl, libsqlite3, libz. No libsecret/keyutils/tpm2-tss names.
- Caveat: a sysroot listing shows what the SDK ships, not what the device ships.

### L4 - Probe for the 3.28.0.172 / 5.8.203 installer URL
- Tried `.../3.28.0.{140,150,157,162,172,176}/ferrari/...5.8.203-ferrari-public-x86_64-toolchain.sh` and `3.28.1.0` and aarch64 variant: all HTTP 404; bucket listing denied. Status: negative result. The 5.8.203 URL remains **UNVERIFIED**.

### L5 - gist JustSimplyKyle (older ferrari SDK)
- URL: https://gist.github.com/JustSimplyKyle/cf4a8ceff2763d82a2815e5c8c27dc3d. Status: **VERIFIED [3P]**.
- `/opt/codex/ferrari/4.0.813/environment-setup-cortexa53-crypto-remarkable-linux`; CMake file `.../sysroots/x86_64-codexsdk-linux/usr/share/cmake/cortexa53-crypto-remarkable-linux-toolchain.cmake`; a commenter used `meta-toolchain-remarkable-4.1.112-ferrari-public-x86_64-toolchain.sh`. Older than 5.7.119; useful only to confirm the cortexa53 naming is long-standing.

### L6 - developer.remarkable.com pages (via search only)
- Pages: /documentation/sdk, /documentation/qt_epaper, /documentation/developer-mode, /documentation/recovery-mode, /documentation/recovery-for-linux-host, /links. Status: **SNIPPET** (site unreachable).
- SDK page: each product needs its own SDK ("exactly that product"); only x86_64 host SDKs historically; from 3.27.0.97 x86_64 and aarch64 hosts; installers are shell scripts (set exec bit; `-d` dir flag mentioned).
- /links table (snippet): `3.28.0.172 | ferrari | remarkable-production-image-5.8.203-ferrari-public-aarch64-toolchain.sh` (and x86_64); same 5.8.203 for chiappa, tatsu, rm2, rm1; opening line still names 5.7.119 (may be stale). Also listed: ferrari 3.24.2.0 (5.4.107), 3.23.0.64 (5.3.95), 3.22.0.65 (5.2.96).
- qt_epaper guide (snippet): pure Qt Quick only (no Widgets); Qt6 + CMake; `qt_standard_project_setup(REQUIRES 6.5)`, `qt_add_qml_module`; touch works out of the box, marker handling more involved; one summary says the run step is "stop UI service, `QT_QUICK_BACKEND=epaper ... -platform epaper`, restart" and another said the launcher code sets no env vars. **Conflicting summaries; treat as unconfirmed.** On 3.17 `libqsgepaper.so` had to be copied manually.
- Developer mode (snippet): enabling factory-resets; "security ... fundamentally weakened"; breaks secure-boot chain of trust; disables most of secure boot but not disk encryption; warning on every boot; SSH credentials under Settings > General > Help > About > Copyrights and licenses; modifications may void warranty for resulting defects; recovery mode needed to leave; reset changes SSH password and removes keys (community note).

### L7 - remarkable.guide (toolchains, developer-mode)
- Pages blocked (ENOTFOUND). Status: **SNIPPET** only: lists official installers by OS (4.0.813 for 3.14.1.10-3.14.3.0; 4.0.117-1 for 3.6-3.8). Nothing newer taken from it.

### L8 - remagic repo (README, APP-SPEC, DEVELOPER-MODE, LICENSE, sources.env, catalog.json, cli/README)
- URLs: https://github.com/MaximeRivest/remagic and raw.githubusercontent.com/maximerivest/remagic/HEAD/{README.md,docs/APP-SPEC.md,docs/DEVELOPER-MODE.md,LICENSE,scripts/sources.env,catalog.json,cli/README.md}. Status: **VERIFIED [upstream]**.
- MIT, copyright 2026 Maxime. Installer = `get.sh`/`get.ps1` -> `remagic setup`: checks dev mode, installs SSH key, xovi + AppLoad from upstream, xovi-tripletap (triple power press toggles xovi; "can't bootloop you"), Store app. Re-run after every OS update. Autostart script is opt-in and "riskier (upstream advises against naive autostart on the encrypted device)".
- `sources.env` pins `XOVI_BUNDLE_TAG=v19-23052026` and `APPLOAD_TAG=v0.5.3` (the problematic AppLoad on 3.28).
- App format: see COMPATIBILITY.md section 2 (manifest fields; `external.manifest.json` + `icon.png`; apps in `/home/root/xovi/exthome/appload/<app>/`; Reload in AppLoad; catalog entry `{id,version,url,sha256}`; publish via `gh` release; human-reviewed PR since apps run as root).
- Dev-mode walkthrough: path Settings > General > Paper Tablet > Software > Advanced > Developer Mode; confirm by pressing power twice; SSH password in Settings > Help > Copyrights and licenses; `ssh root@10.11.99.1` over USB; bootloader stays signed, disk encryption stays on, boot notice stays; recovery via `rm_recover` 1.14.1 (`discover-device`, `reset`/`restore`, both disable dev mode). Claims dev mode does not void warranty (conflicts with L6 snippet). SSH-over-Wi-Fi toggle: Settings > General > Software > Advanced.
- README says "Ferrari / Chiappa / Tatsu - i.MX8MM" (inconsistent with L1).

### L9 - remagic releases, commits, pull requests
- URLs: .../remagic/releases, .../commits/main, .../pulls. Status: **VERIFIED [upstream]**.
- Latest release: `remagic CLI v0.2.0` (tag `cli-v0.2.0`, 07 Jul 2026); `Store 0.1.1` (06 Jul), `0.1.0`. Latest `main` commit: 13 Jul 2026 `30525a1` ("catalog: add Bad Apple 0.1.0"). Only PR: #3 "Fix setup on OS 3.27+ (hashtab) and 3.28+ (AppLoad hooks)", open since 2026-07-08. Release years inferred from commit/issue dates.

### L10 - remagic issues (#2, #5, #7, #11, #12, #13 and index)
- URLs: https://github.com/MaximeRivest/remagic/issues/{2,5,7,11,12,13}. Status: **VERIFIED [upstream]** (issue text); fix claims are user reports.
- Index (12 open): #13 3.29.0.153 compat question (2026-10-18, no replies); #12; #11; #10/#9/#8/#6 checksum mismatches; #7; #5; #4 overwrites `~/.ssh/id_ed25519`; #2; #1 Riddle on Move.
- #11 (2026-09-16, Paper Pro 3.28.0.172): triple-press crashes xochitl; setup: "couldn't rebuild non-interactively for the hashtab"; manual rebuild froze >10 min. Comment 2026-09-25: setup pins v0.5.3 whose hooks do not resolve on 3.28 (rm-appload#65); workaround = `rebuild_hashtable` (Enter, passcode on tablet, wait "Hashtab saved") + replace `extensions.d/appload.so` with v0.6.0 (adds 3.28 per rm-appload#59; **drops <=3.27**) + `xovi/start` ("Loaded external AppLoad hooks in main UI"). Riddle also needs `libquill.so` fix (quill#5).
- #12 (2026-10-14, 3.28.0.162, AppLoad v0.6.0, Riddle 0.3.0): launching Riddle terminates xochitl; takeover service stops xochitl; display never returns. No resolution.
- #2: on 3.27.3.0 AppLoad panics without hashtab, systemd gives up, device reboots to stock; `remagic doctor` false green; commit `a99f90e` (2026-10-18) "Fix setup on OS 3.27+ (hashtab) and 3.28+ (AppLoad hooks)" linked, merge not shown.
- #5: xovi never injects (build 20260612085811, 3.27.1.0, 3.27.3.0, 3.28.0.162); fixed for two commenters by `rebuild_hashtable` + `xovi/start`.
- #7: three friction points on 3.27.3.0 (hashtab required, stale riddle checksum, exec bit lost).

### L11 - asivery/rm-appload (README, releases, issues #59 #65 #75 #82)
- URLs: https://github.com/asivery/rm-appload{,/releases,/issues,/issues/59,/issues/65,/issues/75,/issues/82}. Status: **VERIFIED [upstream]**; release years inferred.
- README: XOVI extension for custom windowed/fullscreen apps; QML frontend + optional backend in any language; targets RMPP (mentions rM1, rM2, RMPPM in aspect ratios); depends on XOVI and qt-resource-rebuilder; apps in `/home/root/xovi/exthome/appload/`; drag from centre-top to close; long-press icon for windowed; **GPL-3.0**; AI-written PRs rejected. Manifest fields: see COMPATIBILITY.md section 2. `qtfb` passes `QTFB_KEY`.
- Releases: v0.6.0 (Latest, 19 Sep, "Grab focus on QTFB window creation"), v0.5.3 (21 May, "shim: Make it possible to shim different models"), v0.5.2 (08 May, "fix: compatibility with 3.27 (#48)"), v0.5.1 (18 Apr), v0.5.0 (29 Mar), v0.4.2 (20 Mar), v0.4.1 (04 Dec, "Add support for version 3.24 (#35)"), v0.4.0 (15 Nov, "Implement refresh mode switching and full refresh request from apps"). Notes are commit titles only; no crash-loop text.
- #59 "support for 3.28" (rmitchellscott, draft 2026-06-19, ready 2026-09-03, merged 2026-09-07, commit 3b42440): "breaking for <=3.27"; tested on Paper Pro Move 3.28.0.157; rM2 3.28.0.162 beta with v0.5.3 panicked with unresolved hashed identifier.
- #65 (Paper Pro 3.28.0.162, v0.5.2 and v0.5.3): `Couldn't resolve the hashed identifier 4073320026945606142 required by AppLoad hooks in main UI`; hashtab rebuild (20,342 entries) did not help; reboot returns to stock. Closed 2026-07-23 (reason not shown).
- #75 "AppLoad not appearing with 3.28": maintainer said support existed but unreleased; closed 2026-09-20 after a working release was published; recommended remanager.io for upgrades.
- #82 (open, 2026-10-09): v0.6.0 race in `terminate()` on close, reproduced on Paper Pure.
- Open issues as of retrieval: #82, #80, #79, #53 (QPAs), #41, #33.

### L12 - asivery/xovi and asivery/rm-xovi-extensions
- URLs: https://github.com/asivery/xovi, https://github.com/asivery/rm-xovi-extensions (+/releases). Status: **VERIFIED [upstream]**.
- xovi: LD_PRELOAD/dlsym function-hook framework; aarch64 supported (Paper Pro prebuilts), arm32 testing; LGPL-3.0; layout `/home/root/xovi/{extensions.d,exthome}`; no OS list; Releases empty in capture.
- rm-xovi-extensions: GPL-3.0; extensions: qt-resource-rebuilder (default), webserver-remote, modules framebuffer-spy/qt-command-executor/xovi-message-broker; **hashtab rebuild required after OS update** (`xovi/rebuild_hashtable`); **"DO NOT attempt to set up XOVI to start automatically on boot"** on encrypted tablets - editing xochitl systemd files causes a bootloop needing manual recovery. Releases: `pre-v20-08092026` (pre-release 08 Sep), `v19-23052026` (Latest 23 May), `v18-23032026`, `v17-14012026`, `v16-14112025`, `v14-20092025`.
- Not stated: supported OS versions.

### L13 - xovi-tripletap, vellum-cli, qmldiff, freeMarkable, rmitchellscott/rm-appload
- URLs: github.com/rmitchellscott/xovi-tripletap, vellum-dev/vellum-cli, asivery/qmldiff, wowitsjack/freeMarkable, rmitchellscott/rm-appload. Status: **VERIFIED [upstream]**.
- tripletap: GPL-3.0; starts/restarts xovi on N power presses; lists rMPP, rM1, rM2, Move, Pure; "USE AT YOUR OWN RISK". vellum-cli: MIT; apk-tools fork, state in `/home/root/.vellum/`, `vellum check-os`; (reManager GUI mentioned in L15). qmldiff: GPL-3.0; `.qmd` = QML diff files (`AFFECT/TRAVERSE/LOCATE/INSERT/REPLACE`), hashtab maps hashed names back; used by qt-resource-rebuilder (relation inferred from L12). freeMarkable: MIT, GUI installer, Paper Pro "Fully Supported", 3.28 unstated; v1.1.1b. rmitchellscott/rm-appload: fork, `master` only visible, no releases.

### L14 - usetrmnl/trmnl-remarkable #19
- URL: https://github.com/usetrmnl/trmnl-remarkable/issues/19. Status: **VERIFIED [upstream]**.
- rM2 3.27.3.0, XOVI 0.3.3, ext 19.0.0, AppLoad 0.5.3: missing hashtab -> xochitl aborts, systemd restarts, device reboots. Fix: `rebuild_hashtable` (~660 KB) before start. Installer reports success even if xochitl crashes; check PID stable and `NRestarts=0`. Not a Paper Pro, same mechanism.

### L15 - snydersaurus/remarkable-mlb-scoreboard (README, docs/PLATFORM.md, package.sh, manifest)
- URLs: raw.githubusercontent.com/snydersaurus/remarkable-mlb-scoreboard/HEAD/{README.md,docs/PLATFORM.md,package.sh,appload-native/manifest.json}. Status: **VERIFIED [3P]**, MIT. Device tested: Paper Pro **Move** (chiappa) "Codex Linux 5.7.126 / Qt 6.8.2"; ferrari "expected, untested".
- SDK must be at or below device Qt (6.10 build fails on 6.8.2). "SDK 5.8.203 ships Qt 6.10; 5.7.119 ships 6.8.2" (claim read; 5.8.203 not inspected by me). SDKs public no login at developer.remarkable.com/links, ~460 MB.
- Native AppLoad app layout/manifest/startup caching; frontend/backend split; socket protocol (SEQPACKET, 8-byte header, always read payload datagram); backend launched with bare environment; `scp` ETXTBSY; xochitl restart needed for new apps (~1 min, lock screen).
- Clock UTC; deep suspend ~40 s; wakelock `/sys/power/wake_lock`; USB/Wi-Fi down in deep sleep; BusyBox (no curl); `/usr` read-only, use `/home/root`.
- TLS: ECDHE-ECDSA-only policy in `10-reduce-tls-ciphers.cnf`; workaround `OPENSSL_CONF` set in backend with `qputenv`.
- "Do not retry" note: external app via qtfb + linuxfb does not work on Paper Pro (no linuxfb plugin, no `/dev/fb0`, shim models only RMPP/RMPPM/CHIAPPA/FERRARI).
- E Ink rules (no animations/gradients/dark fills; greys; 6.5 mm targets; glyph gaps; no kinetic flicks). Distribution: prebuilt zip; settings outside app dir; Vellum PRs must be human-written.

### L16 - exp78/rmweb (README, docs/install.md, toolchain/README.md, docs/device-profile.md, scripts/fetch-sdk.sh) and jhasubhash/rmbrowser
- URLs: github.com/exp78/rmweb, raw .../HEAD/{README.md,docs/install.md,toolchain/README.md,docs/device-profile.md,scripts/fetch-sdk.sh}; github.com/jhasubhash/rmbrowser. Status: **VERIFIED [3P]**, MIT.
- Device profile, Paper Pro ferrari, verified over SSH 2026-06-24, re-verified 2026-08-26 on OS **3.28.0.164**: i.MX8M Mini, 4x Cortex-A53, ~2 GB RAM + 2.5 GB swap, Codex Linux 5.8.199 (scarthgap), kernel 6.12.49, glibc 2.39, BusyBox 1.36.1; rootfs 500 MB ~91% full; `/home` 46 GB on `/dev/mapper/home-encrypted-disk`; no `/dev/fb*`, DRM `imx-drm` mode 405x1084 packed to 1620x2160 ARGB8888 Gallery 3; no GPU driver; xochitl = Qt 6.10.3 Quick, presents via `/usr/lib/plugins/platforms/libepaper.so`; QPA plugins: offscreen, minimal, vnc, epaper. Input: event2 Elan marker (pen), event3 Elan touch (X 0..2064, Y 0..2832); resolve by name.
- Toolchain: triple `aarch64-remarkable-linux`, `-mcpu=cortex-a53+crc+crypto`, GCC 13.4.0, sysroot `cortexa53-crypto-remarkable-linux`, env `source /opt/rmpp-sdk/environment-setup-cortexa53-crypto-remarkable-linux` (Docker install path); pinned SDK URL 5.7.119 with sha256 `218847107b9fca7bff8fb92db01ef5f74ec3df283a9ff0fe3a6e62b64a8237b4`; SDK 3.27.0.97 ABI-compatible with 3.28.x.
- install.md: stops xochitl while browser is on screen and restarts on exit/crash (trap), SIGKILL leaves xochitl stopped until reboot; AppLoad icon via XOVI; verified on Paper Pro OS 3.28 (beta) 2026-07-26; **"v0.5.3 only supports <=3.27 and crash-loops xochitl"**, 3.28 needed appload.so from PR #59 (branch `3.28` of rmitchellscott/rm-appload).
- rmbrowser: fork of rmweb, "Paper Pro only, OS 3.28 or later", one e-ink update per page. rmweb v0.9.7 beta, WPE WebKit, CPU only, no JIT; reader/animation freezing; B&W fast mode.

### L17 - Techthos/remarkable (CLAUDE.md, Makefile)
- URLs: raw.githubusercontent.com/Techthos/remarkable/HEAD/{CLAUDE.md,Makefile}. Status: **VERIFIED [3P]**, MIT; device is a **rM2** (armv7) on OS 3.28.0.172, xochitl Qt 6.10.3.
- SDK path `~/.local/opt/remarkable-sdk/3.28.0.172`, env `environment-setup-cortexa7hf-neon-remarkable-linux-gnueabi` (rM2).
- Run command with `systemctl mask --runtime remarkable-fail.service; systemctl stop xochitl; QT_QUICK_BACKEND=epaper ... -platform epaper; ...unmask; systemctl start xochitl`. `xochitl` has `OnFailure=remarkable-fail.service` which reboots the tablet; every xochitl start re-asks the passcode although `/home` stays unlocked; `systemctl freeze` unsupported (hybrid cgroup); xochitl holds flocks on `/tmp/epframebuffer.lock` and `/tmp/epd.lock`; `/home/root/.config/remarkable/xochitl.conf` holds the root password; "Toltec and remarkable-hacks do not support OS 3.28; current stack is Vellum, xovi, AppLoad (3.28 support merged September 2026)".

### L18 - Jayy001/RmAnki
- URL: https://github.com/Jayy001/RmAnki. Status: **VERIFIED [3P]** (rM1/rM2 only), GPL-3.0.
- Run: `QT_QUICK_BACKEND=epaper /home/root/anki -platform epaper` after `systemctl stop xochitl`; rM1 `QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS="rotate=180"`, rM2 `"rotate=180:invertx"`; mentions "if you're on version 3.17, read the developer docs".

### L19 - reMarkable/epaper-qpa
- URL: https://github.com/reMarkable/epaper-qpa (+ `epaperintegration.h`). Status: **VERIFIED [vendor]** for header contents only. Old "quickly hacked together qpa based on qminimal" (branch `new_devices`), Qt LGPL headers, static dpi 228, evdev touch and keyboard handlers; no refresh API visible. Not the production Paper Pro plugin (that is closed `libepaper.so`). Low value.

### L20 - reTaskable, ankimarkable
- URLs: https://github.com/jdkruzr/reTaskable, https://github.com/gittyhubbycoder/ankimarkable. Status: **VERIFIED [upstream]** (reTaskable); ankimarkable fetch returned README text for an "OpenRouter Chat" app (AGPL-3.0, Rust/zigbuild, AppLoad external manifest with `qtfb: true` and `environment` passing `OPENROUTER_API_KEY`; key stored plaintext in the manifest), which does not match the repo name - treat as **SNIPPET-grade**, repo identity unclear.
- reTaskable: GPL-3.0, QML + Rust backend, xovi/qmldiff hooks for 3.27.x only, Vellum/reManager install, `~/.config/retaskable/config.toml` with plaintext app password, discrete paging with up/down buttons.

### L21 - KOReader Paper Pro issue and KOReader notes
- URL: https://github.com/koreader/koreader/issues/13678 (through July 2025); search snippets of KOReader repo history. Status: **VERIFIED [upstream]** (issue) / **SNIPPET** (KOReader wiki, forum).
- 3.19 beta / 3.20 broke qtfb/AppLoad 0.1.0; touch near the bottom registers higher (unresolved); no suspend/Wi-Fi/pen findings. Snippet: on Paper Pro Move, closing folio or pressing power while KOReader runs leaves a "Sleeping" modal; firmware updates wipe KOReader systemd units.

### L22 - Mail clients and triage apps
- Epistolary: https://github.com/j6k4m8/epistolary. **VERIFIED [upstream]**: "Hand-written email client on a reMarkable or tablet of your choice"; Python (uv), IMAP/SMTP, `rmapi`, Tesseract or OpenAI OCR; Apache-2.0; 25 commits, 62 stars; last commit 2025-02-06; PyPI package; no inline images/attachments; Paper Pro not mentioned. Name check: a search for "Epistolary j6k4m8" first surfaced `remailable` (email PDF -> tablet, Apache-2.0, Python) and `goosepaper`; PaperEmail (sr.ht `~roselandgoose/PaperEmail`, rM2). Those are **SNIPPET**.
- rmbrowser: https://github.com/jhasubhash/rmbrowser (MIT, see L16). exp78/rmweb (MIT).
- GitHub repo search "rmbrowser": 1 result (jhasubhash/rmbrowser). Search "remarkable email imap": **0 results**. Web searches for an on-device Paper Pro IMAP/mail client: **no evidence found** (not a proof that none exists).
- Related AppLoad apps found: reTaskable (CalDAV), rmpp-split-doc (MIT, qmd patches, firmware 3.27.x only; 3.28+ "Not supported"), remarkable-remote-share (XOVI + Tailscale), freeMarkable, KOReader.

### L23 - Latest OS version evidence
- Notebookcheck and Techwalla (snippets), RCU page (snippet), remagic issues (L10), rm-appload (L11), SDK table (L6). Status: **SNIPPET** for vendor claims; **VERIFIED [upstream]** for versions seen in issues.
- 3.28 shipped (Capture tool, new pen colours, EPUB refresh) - news snippets; exact date not confirmed (release-notes page unreachable). RCU v5.1.1 (2026-08-20) compat up to 3.28.0.172 (snippet). Build numbers seen on Paper Pro family: 3.27.0.97 (SDK), 3.27.1.0, 3.27.3.0, 3.28.0.157 (Move beta), 3.28.0.162, 3.28.0.164, **3.28.0.172** (links table and user reports), and **3.29.0.153** (user in remagic #13, 2026-10-18; channel unknown). Conclusion: latest confirmed stable ~3.28.0.172; 3.29 exists in the wild, unverified.

### L24 - Jayy001/codexctl
- URL: https://github.com/Jayy001/codexctl. Status: **VERIFIED [upstream]**. CLI for firmware install/download/backup via ddvk update server; `rmpp`/`rmppm` hardware options; cannot downgrade from >=3.11.2.5 to a lower version; auto-handles bootloader at the 3.20/3.22 boundary; no developer-mode text.

### L25 - Secure storage, clock, suspend, Wi-Fi searches
- Queries on keyring/Secret Service/TPM for reMarkable and on suspend/Wi-Fi/NTP/RTC for Paper Pro returned only generic Linux material. Status: **no evidence found**. All usable data came from L15-L17 (Move/Paper Pro/rM2 third-party notes). NTP, RTC retention, Wi-Fi resume time on Paper Pro: **UNVERIFIED**.

### L26 - Other negative or unreachable lookups
- developer.remarkable.com documentation (SDK, qt_epaper, developer-mode, links), release notes pages, Medium Todoist write-up ("QT_QPA_PLATFORM=epaper + QT_QUICK_BACKEND=epaper", snippet only), dragly.org 2017 post (snippet: `QMLSCENE_DEVICE=epaper`, `QT_QPA_PLATFORM=epaper:enable_fonts`, stop xochitl; rM1-era, dated): all SNIPPET or unreachable.
- Official statement on xochitl stop/restart risk on encrypted Paper Pro: not found. Evidence is rM2/Move (L15, L17).

## Items needing a human or a physical device
See COMPATIBILITY.md section 8.
