# Slow Mail

> **Status: pre-alpha. Never run against a real mailbox without reading [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md).**
> Tokens and cached mail are stored unencrypted. Nothing here has been tested on a reMarkable Paper Pro or against a real Microsoft 365 tenant.

## What it is

Slow Mail is a small inbox-triage app for one Microsoft 365 work mailbox, designed for the reMarkable Paper Pro (E Ink) and for processing mail late at night without a notification feed. It is not a mail client: there is no folder browser, search, calendar or composer.

For each message in the Inbox you choose:

| Action | What happens in Outlook |
|---|---|
| **Do / Archive** | The message is moved to the Outlook `archive` folder |
| **Delete** | The message is moved to Deleted Items (never permanently deleted) |
| **Defer** | The message gets a follow-up flag and stays in the Inbox; Slow Mail hides it from its queue |
| Delegate | Not in the MVP |

Rules that shape the app (full list: [docs/DECISIONS.md](docs/DECISIONS.md)):
- Inbound download is manual and limited to one fresh download per four hours (measured from the last *complete* successful sync). Cached mail is always readable. Archive, delete and defer are never held back by that limit and are queued if you are offline.
- An action is shown as done only after Microsoft Graph confirms it; otherwise it is Pending or Failed.
- Attachments are listed (name, type, size) and never downloaded. Message bodies are shown as sanitised plain text.
- No background sync, no push, no analytics.

The domain logic is plain C++17 without Qt. Only the UI uses Qt Quick.

## Repository layout

```
src/core/        Qt-free C++17: types, session state machine, cooldown, sync engine, command queue, sanitiser, logging
src/storage/     SQLite implementation of the core Store (WAL, synchronous=FULL, file mode 0600)
src/graph/       Microsoft Graph adapter and device-code OAuth, libcurl transport
src/ui/          Qt Quick (QML) desktop simulator / device UI
src/platform/    Data directory and device detection
src/cli/         slowmail-cli: headless driver for the whole stack
tests/unit/     GoogleTest unit tests: core/, storage/, graph/   (core_tests, storage_tests, graph_tests)
tests/contract/ Contract tests against the fake Graph server     (contract_tests)
tests/device/   Manual device checklist (CHECKLIST.md)
tools/fake_graph/  Python fake Microsoft identity platform + Graph server
packaging/      Device launch wrapper and DRAFT launcher manifests (UNTESTED)
scripts/        build-host.sh, run-simulator.sh, and UNTESTED device scripts
third_party/    Vendored nlohmann/json (used when no system copy is found)
docs/           Design, decisions, risks, threat model, test plan, research
archive/        Earlier contents of this repository, kept for reference
```

## Build (Ubuntu 24.04, desktop simulator)

```sh
sudo apt-get install cmake ninja-build g++ qt6-base-dev qt6-declarative-dev \
  qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-templates qml6-module-qtquick-window qml6-module-qtqml-workerscript \
  libgtest-dev nlohmann-json3-dev libcurl4-openssl-dev libssl-dev libsqlite3-dev

cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
```

`scripts/build-host.sh` does the same. The UI needs Qt 6.4 (the Ubuntu 24.04 version); without Qt the core, storage, graph, CLI and tests still build. Python 3 is needed for the contract tests. Options: `-DSLOWMAIL_BUILD_UI=OFF`, `-DSLOWMAIL_BUILD_TESTS=OFF`, `-DSLOWMAIL_BUILD_CLI=OFF`, `-DSLOWMAIL_WARNINGS_AS_ERRORS=ON`.

## Run the simulator against the fake server

No Microsoft account is involved.

```sh
python3 tools/fake_graph/server.py --port 8765 --generate 50 --auto-approve-after 2
```

In a second terminal:

```sh
SLOWMAIL_GRAPH_BASE_URL=http://127.0.0.1:8765/v1.0 \
SLOWMAIL_AUTHORITY=http://127.0.0.1:8765 \
SLOWMAIL_CLIENT_ID=fake \
./build/src/ui/slowmail
```

The CLI equivalent (same environment variables): `./build/src/cli/slowmail-cli --help`, for example `slowmail-cli signin`, `refresh`, `queue`, `archive <id>`.

`scripts/run-simulator.sh [ui|cli] [-- args]` starts the fake server on a free port, sets these variables, uses a temporary data directory and runs the program. Run `python3 tools/fake_graph/server.py --help` for the failure modes it can simulate (no archive folder, partial delta updates, the immutable-id delta gap, short token lifetime, and others). On a machine without a display use `QT_QPA_PLATFORM=offscreen`.

## Environment variables

| Variable | Meaning |
|---|---|
| `SLOWMAIL_CLIENT_ID` | Application (client) id of your Entra app registration. Required for real sign-in. **Never commit it.** |
| `SLOWMAIL_TENANT_ID` | Tenant id or `organizations` (default). **Never commit it.** |
| `SLOWMAIL_DATA_DIR` | Where the SQLite database lives. Default: `$XDG_DATA_HOME/slowmail` or `~/.local/share/slowmail` on a host; `/home/root/.local/share/slowmail` on the device |
| `SLOWMAIL_GRAPH_BASE_URL` | Graph base URL. Default `https://graph.microsoft.com/v1.0`. Override only for the fake server |
| `SLOWMAIL_AUTHORITY` | Login authority. Default `https://login.microsoftonline.com`. Override only for the fake server |
| `SLOWMAIL_PRO=1` | Lift the four-hour cooldown (the "Pro" concept; testing only, no payment code exists) |
| `SLOWMAIL_COOLDOWN_SECONDS` | Shorten the cooldown for testing (read by the CLI; check the UI before relying on it) |
| `SLOWMAIL_CA_BUNDLE` | Path to a CA bundle for TLS (verification cannot be turned off) |

## Microsoft Graph consent scopes

Delegated, requested at sign-in by the device-code flow:

- `offline_access` (refresh token)
- `User.Read`
- `Mail.ReadWrite`

`Mail.Send` is requested only when sending exists (later); adding it needs a new sign-in. No application permissions. The app registration must have "Allow public client flows" enabled. Details and the unverified parts: [docs/GRAPH_CONTRACT.md](docs/GRAPH_CONTRACT.md) section 1.

Device-code sign-in may be blocked by your tenant's Conditional Access policy (Microsoft recommends blocking it). That is a conversation with your tenant admin; the app has no workaround and will show an error instead.

## Security notice

- Refresh and access tokens are stored as plaintext inside a file-mode-0600 SQLite database. There is no encryption at rest, no hardware-backed key storage, no token binding and no remote wipe. The reMarkable Paper Pro in developer mode (required for custom apps) has weakened secure boot.
- Do not use a real work mailbox until the hard gates in [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) section 7 are met: device-owner approval, tenant-admin approval, a security decision on plaintext tokens, and a synthetic-tenant run first.
- Sign-out clears the local token and, if you choose, wipes the cache. It does not revoke the token at Microsoft; see the revoke and lost-device runbook in the threat model.
- Logs never contain tokens, subjects, bodies or addresses (a `redact()` safety net exists); do not paste database contents into bug reports.

## Documentation

| Document | Contents |
|---|---|
| [docs/DECISIONS.md](docs/DECISIONS.md) | Decisions D1-D24 (product-owner decisions on the handover's Q1-Q16, plus engineering decisions) |
| [docs/ASSUMPTIONS.md](docs/ASSUMPTIONS.md) | Numbered assumptions with VERIFIED / UNVERIFIED / FALSIFIED status |
| [docs/RISKS.md](docs/RISKS.md) | Risk register |
| [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) | Assets, actors, STRIDE, controls, hard gates, revoke runbook |
| [docs/GRAPH_CONTRACT.md](docs/GRAPH_CONTRACT.md) | What Microsoft Graph and Entra do, with a status tag per claim |
| [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md), [docs/RESEARCH_LOG.md](docs/RESEARCH_LOG.md) | reMarkable Paper Pro SDK, launcher and OS compatibility research |
| [docs/TEST_PLAN.md](docs/TEST_PLAN.md), [docs/TEST_LOG.md](docs/TEST_LOG.md), [tests/device/CHECKLIST.md](tests/device/CHECKLIST.md) | Acceptance matrix A01-A20, results log, device checklist |
| [docs/LICENSES.md](docs/LICENSES.md) | Dependency licences |
| [packaging/README.md](packaging/README.md) | Device packaging (UNTESTED) |

## What was tested where

| Level | Status |
|---|---|
| Unit tests (core, storage, graph adapter) on the Ubuntu host | Written; results are recorded in [docs/TEST_LOG.md](docs/TEST_LOG.md) (integrator fills in). See [docs/TEST_PLAN.md](docs/TEST_PLAN.md) for which acceptance items have an automated test |
| Desktop simulator (Qt 6.4 UI on the host) | Built and run by hand on the host; see TEST_LOG. No automated UI tests |
| Fake Graph server (contract tests, CLI) | Python fake server in the repo; contract tests: see TEST_PLAN, which lists what is not yet written. The fake server encodes our model of Graph, not Microsoft's behaviour |
| Real Microsoft 365 mailbox or synthetic tenant | **NOT YET** |
| reMarkable Paper Pro device | **NOT YET.** Cross-compile and install scripts exist but are UNTESTED; the build environment had no SDK |

## Licence

No licence has been chosen for Slow Mail yet (see [docs/DECISIONS.md](docs/DECISIONS.md) D16 and [docs/LICENSES.md](docs/LICENSES.md)); until then all rights are reserved. Dependency licences are listed in docs/LICENSES.md.
