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

## 2026-10-20 - Adversarial review round: fixes and re-run (host + aarch64 under QEMU)

| Field | Value |
|---|---|
| Date / time (UTC) | 2026-10-20 20:05-20:50 |
| Tester / agent | Claude (independent review agent, three fix agents, integrator re-ran everything from clean build directories) |
| Commit id | a9f808f (review fixes) plus this log entry |
| Level | simulator + fake server (host); fake server (aarch64 under QEMU). Real mailbox: NOT YET. Device: NOT YET. |
| Environment | as in the previous entry |

Review findings (18 raised; severity per the reviewer) and outcome:

| # | Finding | Outcome |
|---|---|---|
| 1 | High: one network error left the session Offline for good; nothing re-sent pending commands; refresh refused | Fixed: actions, flush and refresh always attempt delivery; any server answer clears Offline; refresh flushes first; start() makes one bounded flush; UI "Send pending (N)" button; tests `TriageSession.*AfterOneNetworkBlip*`, `*RefreshFromOffline*` |
| 2 | High: a Failed message vanished from every list with no retry | Fixed: `failed()`/`retry()` in the facade, Failed section with Retry in the UI, `failed`/`retry` CLI commands; Server/Forbidden/Conflict stay Pending with capped backoff instead of failing after 8 attempts; a successful sync un-hides a Failed row |
| 3 | High: cooldown used CLOCK_MONOTONIC, which stops in suspend | Fixed: CLOCK_BOOTTIME, and eligible when either monotonic or wall elapsed reaches the cooldown (D23 revised) |
| 4 | Medium: SQLite kept the tombstone when a gone message returned, so it was hidden and purged; MemoryStore did not | Fixed in `set_message_state` (schema v2 migration repairs old rows); new `integration_tests` target runs engine scenarios over MemoryStore and SqliteStore |
| 5 | Medium: a 404 with no Graph code counted as "already gone" (core and partial-item follow-up) | Fixed in both places: only `ErrorItemNotFound` means gone; empty code is retryable; non-JSON bodies labelled |
| 6 | Medium: any changed delta entry resurrected Archived/Deleted/Gone rows regardless of folder; changes applied before removals | Fixed: Inbox folder id learned from full enumeration, restore only when parent folder matches; removals first |
| 7 | Medium: local write failure after server confirmation left the command InFlight until restart | Fixed: retry the write once then back to Pending; `process()` recovers InFlight first; park/fail writes checked |
| 8 | Test validity: A06 passed only because the fake treats a re-move as a no-op | Fake gained `--remove-already-moved-404`; a second A06 contract test asserts the outcome in that mode (no duplicate, no loss, undo unavailable); ASM-34 records the open question |
| 9 | Low: same-second commands ordered randomly in SQLite | Fixed: order by rowid on ties |
| 10 | Low: tenant policy block at the token endpoint reported as a generic error | Fixed: Forbidden with AADSTS code; distinct UI/CLI message |
| 11 | Low: a stored delta link from another origin stranded sync | Fixed: mapped to Gone (full resync), token never sent |
| 12 | Low: `not_before` on wall time could park a command far in the future | Fixed: clamped on read |
| 13 | Low: worker thread sleeps on throttling; shutdown waits on it | Not fixed; recorded as R-WORKER-BLOCKING |
| 14 | Low: `set_flag` confirmed on any 2xx | Fixed: response `flagStatus` validated |
| 15 | Low: no single-instance lock on the data directory | Not fixed; recorded as R-NO-INSTANCE-LOCK |
| 16 | Low: `<!-->` / `<!--->` swallowed text; some invisible code points passed | Fixed with tests; 12k-input fuzz by the reviewer found no unsafe output before or after |
| 17 | Low: fake-server request log capped at 200; scope never checked | Fixed: counters independent of the log; Mail.ReadWrite enforced for writes |
| 18 | Nits (poll interval overflow, PlainText, expires_in overflow) | Fixed |

Commands run (same as the previous entry, from clean `build-int` and `build-device`):

```
rm -rf build-int && cmake -S . -B build-int -G Ninja && cmake --build build-int && (cd build-int && ctest)
python3 -I tools/fake_graph/test_server.py
source /opt/codex/ferrari/5.7.119/environment-setup-cortexa53-crypto-remarkable-linux && rm -rf build-device && cmake -S . -B build-device -G Ninja && cmake --build build-device
SLOWMAIL_TEST_TIME_SCALE=10 qemu-aarch64-static -L $SDKTARGETSYSROOT ./build-device/tests/unit/{core,storage,graph,integration}_tests
SLOWMAIL_TEST_TIME_SCALE=10 qemu-aarch64-static -L $SDKTARGETSYSROOT ./build-device/tests/contract/contract_tests
```

Results:

| Item | Host | aarch64 under QEMU |
|---|---|---|
| core_tests | pass 231/231 | pass 231/231 |
| storage_tests | pass 102/102 (+1 skipped as root) | pass 102/102 (+1 skipped) |
| graph_tests | pass 76/76 | pass 76/76 |
| integration_tests (new) | pass 22/22 | pass 22/22 |
| contract_tests | pass 29/29 (17 s) | pass 29/29 (90 s) |
| fake server self-tests | pass 60/60 | n/a |
| ctest | 5/5 targets | n/a |
| Warnings in project sources | none | none |
| CLI end-to-end (fake server) | pass: a dropped move left the archive Pending and the next delete was still delivered; the pending archive was sent on the next start; a missing Archive folder produced a Failed row with `ErrorFolderNotFound`, `failed` listed it, `retry` confirmed it after the folder was restored; final server state inbox 9 / archive 2 / deleteditems 1; all four request-shape counters 0 | n/a |
| Simulator screenshots | regenerated in docs/screenshots/simulator (queue shows "Failed 1" and "Send pending (1)"; activity shows the Failed section with Retry) | UI binary built, not executed |

Deviations, follow-ups: findings 13 and 15 are open risks. The `SLOWMAIL_TEST_TIME_SCALE` knob now also covers the 1 MB sanitiser benchmark. No real-mailbox or hardware run.
