# Slow Mail - Test Log

Append one dated section per run. Do not edit old entries; add a correction entry instead. A result is only valid if it records the commit, environment and exact commands. Unrun items stay as "not run", never blank-as-pass.

Levels: simulator (desktop Qt UI on the host), fake server (contract tests against `tools/fake_graph/server.py`), real mailbox, device. See [TEST_PLAN.md](TEST_PLAN.md).

## Template (copy this block for each run)

### YYYY-MM-DD - <short title>

| Field | Value |
|---|---|
| Date / time (UTC) | |
| Tester / agent | |
| Commit id (`git rev-parse HEAD`) and dirty? | |
| Level | simulator / fake server / real mailbox / device |
| Host OS, compiler, CMake, Qt version | |
| Device model / OS build / SDK / launcher versions (device runs only) | |
| Mailbox | none (fake) / synthetic tenant / real (should be NOT YET) |

Commands run:

```
<exact commands>
```

Results:

| Item | Result (pass / fail / not run) | Evidence (log excerpt, file, photo) |
|---|---|---|
| core_tests | | |
| storage_tests | | |
| graph_tests | | |
| contract_tests | | |
| A01 ... A20 (list each one run) | | |
| Simulator smoke test | | |
| Device checklist items | | |

Deviations from the plan, known failures, follow-ups:

Assumptions updated (ASM-nn changed status):

## 2026-10-20 - First host build, fake-server contract run, aarch64 cross-build under QEMU

| Field | Value |
|---|---|
| Date / time (UTC) | 2026-10-20 19:45-20:05 |
| Tester / agent | Claude (integration pass; module work by sub-agents, re-run by the integrator from a clean build directory) |
| Commit id and dirty? | b20dc0e plus this log entry and the TEST_PLAN reconciliation (committed together) |
| Level | simulator + fake server (host) and fake server (aarch64 under QEMU). Real mailbox: NOT YET. Device: NOT YET. |
| Host OS, compiler, CMake, Qt version | Ubuntu 24.04 (cloud container), GCC 13, CMake 3.28.3, Ninja, Qt 6.4.2, SQLite 3.45.1, libcurl 8.5.0, GoogleTest 1.14, Python 3.13 |
| Device toolchain (cross-build only, no hardware) | reMarkable SDK 5.7.119 for Paper Pro ("ferrari"), `environment-setup-cortexa53-crypto-remarkable-linux`, GCC 13.4, glibc 2.39, Qt 6.8.2, sysroot SQLite 3.45.3, libcurl 8.17.0/OpenSSL 3.2.6; binaries run with `qemu-aarch64-static -L <sysroot>` |
| Mailbox | none (fake server `tools/fake_graph/server.py`) |

Commands run:

```
cmake -S . -B build-int -G Ninja && cmake --build build-int          # host, UI + CLI + tests; zero warnings in project sources
(cd build-int && ctest --output-on-failure)                          # 4/4 targets
python3 -I tools/fake_graph/test_server.py                           # 55 tests
QT_QPA_PLATFORM=offscreen ./build-int/src/ui/slowmail --demo --screenshot-dir docs/screenshots/simulator
# CLI end-to-end against a live fake server (40 generated messages):
python3 tools/fake_graph/server.py --port 0 --generate 40 --seed 7 --auto-approve-after 1
slowmail-cli status | signin | refresh | refresh | queue | open <id> | archive <id> | delete <id> | defer <id> | activity | undo <cmd> | undefer <id> | end | signout --wipe
# device cross-build:
source /opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux
cmake -S . -B build-device -G Ninja && cmake --build build-device    # core, storage, graph, CLI, Qt Quick UI, all tests
SLOWMAIL_TEST_TIME_SCALE=10 qemu-aarch64-static -L $SDKTARGETSYSROOT ./build-device/tests/unit/{core,storage,graph}_tests
SLOWMAIL_TEST_TIME_SCALE=10 qemu-aarch64-static -L $SDKTARGETSYSROOT ./build-device/tests/contract/contract_tests
```

Results:

| Item | Result | Evidence |
|---|---|---|
| core_tests (host) | pass, 187/187 | `[  PASSED  ] 187 tests.` |
| storage_tests (host) | pass, 91/92, 1 skipped | `SqliteStoreFile.SecureWipeFailsWhenUnlinkImpossible` skips when run as root (container runs as root) |
| graph_tests (host) | pass, 66/66 | `[  PASSED  ] 66 tests.` |
| contract_tests (host) | pass, 27/27 in 15 s | ctest `100% tests passed, 0 tests failed out of 4` |
| fake server self-tests | pass, 55/55 | `Ran 55 tests in 6.033s OK` |
| core_tests (aarch64, QEMU) | pass, 187/187 | time-bound assertions need `SLOWMAIL_TEST_TIME_SCALE=10`; without it only `HtmlToText.PathologicalInputsAreLinearAndSafe` fails on wall time (8.5 s vs 3 s), not on behaviour |
| storage_tests (aarch64, QEMU) | pass, 91/92, 1 skipped | same time-scale note for `ThousandMessagesInOneTransactionFastAndOrdered` (3.4-3.9 s vs 2 s under emulation) |
| graph_tests (aarch64, QEMU) | pass, 66/66 | 1.1 s |
| contract_tests (aarch64, QEMU) | pass, 27/27 in 91 s | aarch64 client talking to the host Python fake server over loopback |
| Simulator smoke test | pass | UI starts, renders four pages offscreen at 1620x2160, exits cleanly; screenshots in `docs/screenshots/simulator/` |
| CLI end-to-end | pass | sign-in via device code; first refresh added 40; second refresh exit 2 with next eligible time; archive/delete/defer confirmed; `/__control/state` showed inbox 37 / archive 1 / deleteditems 1 before undo and inbox 39 / archive 0 / deleteditems 1 after undo + undefer; `forbidden_fetch 0`, `content_bytes_served 0`; `end` then `status` opened a new ready_cached session; `signout --wipe` left an empty 0700 data dir; server log contained no token or bearer strings |
| A01, A02, A03, A04, A05, A06, A07, A08, A09, A13, A14, A16, A17, A18, A19 | pass (automated, see TEST_PLAN for test names) | unit + contract runs above |
| A10 | pass locally (sign-out + wipe); server-side revoke not run | needs a tenant |
| A11 | pass at sanitiser level | no end-to-end "second listener" test yet |
| A12, A15, A20 | not run | manual / tenant / post-MVP |
| Device checklist | not run | no hardware in this environment |

Deviations from the plan, known failures, follow-ups:
- Wall-clock bounds in two tests were made scalable via `SLOWMAIL_TEST_TIME_SCALE` so the emulated run is meaningful; the bounds are unchanged on native hosts.
- The aarch64 Qt UI binary builds against SDK Qt 6.8.2 but was not executed (no epaper platform plugin in the sysroot and no display); only the CLI and the test binaries ran under QEMU.
- Nothing was run against Microsoft Graph or a real mailbox. Every Graph behaviour the suite proves is the fake server's model (ASM-33).
- An independent adversarial review of core/graph/storage/UI threading was run after this entry; its findings and fixes are recorded in the next entry.

Assumptions updated: ASM-20 (vendored nlohmann/json) and ASM-21 (cortexa53 env script) confirmed by the successful SDK cross-build; ASM-19 partially: the SDK Qt 6.8.2 build of the UI compiles, runtime unverified.

