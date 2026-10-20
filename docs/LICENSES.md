# Slow Mail - Dependency and Licence Inventory

Status: pre-alpha, not legal advice. Last updated: 2026-10-20. Slow Mail itself has **no licence chosen yet** (owner decision, D16); until one is chosen the source is all-rights-reserved by default.

Evidence tags:
- **VERIFIED** - the licence field was read from the Debian/Ubuntu `copyright` file of the package installed on the build host on 2026-10-20 (Ubuntu 24.04.5), or from the upstream repository by the research agent (RESEARCH_LOG.md). The Debian file shows what the distribution says, not a legal review.
- **FROM-MEMORY** - not checked in this session. Check before relying on it.

Nothing here has had a legal read. Obligations in the "Notes" column are engineering interpretations (UNVERIFIED as legal conclusions).

## 1. Build and runtime dependencies of Slow Mail

| Dependency | Version seen | Licence | How used | Status | Notes |
|---|---|---|---|---|---|
| Qt 6 (Core, Gui, Qml, Quick, QuickControls2) | 6.4.2 on the host (Debian `qt6-base-dev`, `qt6-declarative-dev`); SDK 6.8.2 on the device build (COMPATIBILITY) | Debian copyright: "LGPL-3 or GPL-2" (plus GFDL for docs, GPL-3 with Qt exception for build files); upstream is LGPL-3.0 / GPL / commercial | Dynamically linked, UI only (`src/ui`); core, storage and graph do not use Qt (D17) | VERIFIED (host package); device SDK build FROM-MEMORY | LGPL-3.0 expects dynamic linking, the ability to relink with a modified Qt, licence text and notices. Static linking is not planned. On the device the SDK's Qt `.so` files are used. |
| nlohmann/json | 3.11.3 (`nlohmann-json3-dev`) | MIT ("Expat" in Debian) | Header-only, core and graph | VERIFIED (host package; vendored copy's README and `LICENSE.MIT` present in `third_party/nlohmann_json/`) | The top-level CMake prefers a system copy and falls back to the vendored copy, a verbatim copy of the Ubuntu 3.11.3 package, because the SDK sysroot has no nlohmann/json (per that README). Keep the licence file with the copy. |
| libcurl | 8.5.0 (`libcurl4-openssl-dev`) | curl licence (MIT-like) | HTTP transport, `src/graph` | VERIFIED (host package) | Device build uses the SDK sysroot copy (a listing shows libcurl there, RL L3). |
| OpenSSL | 3.0.13 (`libssl-dev`) | Apache-2.0 | TLS backend of libcurl; also used for random bytes if any | VERIFIED (host package) | OpenSSL 3.x is Apache-2.0. SDK sysroot has `libssl.so.3` (RL L3). |
| SQLite | 3.45.1 (`libsqlite3-dev`) | Public domain | Local store, `src/storage` | VERIFIED (host package) | |
| GoogleTest | 1.14.0 (`libgtest-dev`) | BSD-3-Clause ("BSD-C3" in Debian) | Tests only, not shipped | VERIFIED (host package) | |
| Python 3 standard library | 3.12.3 | Python Software Foundation licence | `tools/fake_graph/server.py`; dev and test only, not shipped; no third-party Python packages are intended | VERIFIED (Debian `python3.12` copyright begins with the Python licence text); "stdlib only" depends on the final server.py | |
| CMake, Ninja, g++ | 3.28.3, 1.11.1, 13.2.0 | BSD-3-Clause (CMake), Apache-2.0 (Ninja), GPL-3.0 with runtime library exception (GCC) | Build tools, not shipped | FROM-MEMORY | The GCC runtime library exception means binaries built with GCC are not themselves GPL. Not checked. |

## 2. Device-side components (not shipped by Slow Mail, relevant to installation)

| Component | Licence | Role | Status | Notes |
|---|---|---|---|---|
| reMarkable SDK (ferrari 5.7.119; installer and sysroot) | Proprietary EULA; sysroot contains many open-source components | Cross-compiler and Qt for the device | Licence text NOT read. Installer header and sysroot listing VERIFIED (RL L2, L3); EULA FROM-MEMORY / unknown | **Check the SDK EULA before redistributing any binary built with it.** The SDK is a download from reMarkable's storage; do not re-host it. |
| xochitl (stock UI) | Proprietary | Stock application; stopped and restarted by the takeover route | Not checked | Not redistributed. |
| remagic (installer, catalog, Store) | MIT (copyright 2026 Maxime) | Optional installer for the launcher stack | VERIFIED (upstream LICENSE read by the research agent, RL L8). The handover says MIT too; the licences of components that remagic downloads are independent. | Check upstream component licences independently (handover section 2, item 5). |
| asivery/rm-appload (AppLoad) | GPL-3.0 | Launcher; native QML module `net.asivery.AppLoad` | VERIFIED (upstream, RL L11) | A native-QML frontend that imports the AppLoad QML module may carry GPL-3.0 obligations; an external process speaking a socket or qtfb probably does not. **Legal inference, UNVERIFIED; get a legal read before choosing Slow Mail's licence or distributing a route B build** (COMPATIBILITY section 2, R-LICENCE-GPL). |
| asivery/rm-xovi-extensions, qmldiff, xovi-tripletap | GPL-3.0 | Launcher stack | VERIFIED (upstream, RL L12, L13) | Not linked into Slow Mail. |
| asivery/xovi (loader) | LGPL-3.0 | Hook framework | VERIFIED (upstream, RL L12) | Not linked into Slow Mail. |

## 3. Reference projects (read, not copied)

| Project | Licence | Status | Note |
|---|---|---|---|
| snydersaurus/remarkable-mlb-scoreboard | MIT | VERIFIED (RL L15) | Architecture and device notes consulted; no code copied. |
| exp78/rmweb, jhasubhash/rmbrowser | MIT | VERIFIED (RL L16) | Notes only. |
| j6k4m8/epistolary | Apache-2.0 | VERIFIED (RL L22) | Concept only. |
| jdkruzr/reTaskable | GPL-3.0 | VERIFIED (RL L20) | Do not copy code into a non-GPL project. |

No third-party source code is vendored in this repository as far as the documentation agent knows. If that changes, add a row.

## 4. Before any distribution

1. Choose Slow Mail's own licence (D16).
2. Read the reMarkable SDK EULA.
3. Get a legal read on the AppLoad GPL-3.0 interaction if route B is used (D24).
4. Produce an SBOM from the actual build (the cross-compiled device build can differ from the host build) and re-verify every FROM-MEMORY row.
5. Include Qt LGPL notices and relinking information for the UI binary.
