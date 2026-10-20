# Slow Mail - Test Plan

Status: pre-alpha. Last updated: 2026-10-20. Results go in [TEST_LOG.md](TEST_LOG.md), not here.

## 1. Test levels and targets

| Level | Where | CMake target / tool | What it proves | What it does not prove |
|---|---|---|---|---|
| Unit: core | `tests/unit/core/` | `core_tests` (ctest name `core_tests`) | Cooldown, session state machine, sync engine, command processor, sanitiser, redaction, with `MemoryStore` and an in-memory fake backend and `FakeClock` | Anything about SQLite, HTTP or Graph |
| Unit: storage | `tests/unit/storage/` | `storage_tests` | SQLite schema, migrations, transactions, 0600 mode, WAL, secure wipe | Power-cut behaviour on real flash |
| Unit: graph | `tests/unit/graph/` | `graph_tests` | Parsers (`parse_delta_page`, `parse_message_summary`), `map_http_error`, URL handling, device-code polling logic against a stub transport | Real Graph or Entra behaviour |
| Contract | `tests/contract/` | `contract_tests` against `tools/fake_graph/server.py` (spawned by the test binary) | The real adapter + storage + core agree with our model of Graph (GRAPH_CONTRACT section 11) | That the model matches Microsoft (ASM-33) |
| Device | `tests/device/` | Manual checklist: [tests/device/CHECKLIST.md](../tests/device/CHECKLIST.md) | Behaviour on a real Paper Pro | Needs the owner, a device and a go decision (D14) |
| Synthetic tenant | not in repo | Manual, same scenarios as contract tests | Real Graph behaviour (ASM-01..ASM-17) | Not scheduled |

Run the automated levels: `ctest --test-dir build --output-on-failure`. List the real test names in a binary with `./build/tests/unit/core_tests --gtest_list_tests` (same for `storage_tests`, `graph_tests`, `contract_tests`; the binaries are under `build/tests/unit/` and `build/tests/contract/`). Filter with `--gtest_filter='<pattern>'`.

**Naming caveat.** This plan was written while the tests were being written by other agents. The "Test name pattern" column is the *planned* GoogleTest suite/test pattern (`Suite.Test*`). The integrator must reconcile each pattern with the output of `--gtest_list_tests` and record the real names in TEST_LOG.md; a row stays "automated" only if a matching test exists and passes. Anything without a matching test is "not yet".

Fixtures: `tests/contract/fixtures/` (path compiled into `contract_tests` as `SLOWMAIL_FIXTURE_DIR`). The meta-tests of GRAPH_CONTRACT 11.9 (immutable header on every request, nextLink never rewritten, no `contentBytes` / `$value`, concurrency bound, `Retry-After` honoured, resync prunes, tolerant parsing, no secrets in logs, refresh-token rotation) apply to every contract scenario.

## 2. Acceptance matrix A01-A20 (handover section 11)

Status: **automated** (a test is planned/expected in the stated target), **manual** (procedure in this document or the device checklist), **not yet** (no test and no procedure ready to run).

| ID | Scenario | Target | Test name pattern | Status | Notes / manual procedure |
|---|---|---|---|---|---|
| A01 | First eligible manual sync at 02:00: inbox fetched, complete delta saved, next eligibility 06:00 | core_tests; contract_tests | `Cooldown.FirstDownload*`; `SyncEngine.Complete*`; `Contract.FirstSync*` | automated | `FakeClock` set to 02:00 UTC; assert `next_eligible_wall` = +4 h and `delta_link` committed only after the last page. |
| A02 | Refresh at 03:00: no inbound Graph fetch, cached queue available | core_tests; contract_tests | `TriageSession.RefreshBlocked*`; `Cooldown.Active*`; `Contract.CooldownNoFetch*` | automated | Assert the fake server request log shows zero delta calls; error kind is `Policy`. |
| A03 | Archive at 03:00: Graph move succeeds despite cooldown, original leaves Inbox | core_tests; contract_tests | `CommandProcessor.Archive*`; `Contract.ArchiveDuringCooldown*`; `Contract.ArchiveFolderMissing*` | automated | Also assert a missing `archive` folder fails visibly and never falls back to `deleteditems` (D4, ASM-01), and that a 404 for the destination is not read as "already gone" (D21, R-NOTFOUND-MISREAD). |
| A04 | Delete at 03:00: message in Deleted Items, not permanently removed | contract_tests | `Contract.DeleteIsSoftMove*` | automated | Assert the fake server's `deleteditems` holds it and no permanent-delete call was made. |
| A05 | Defer at 03:00: flag set, original remains in Inbox, hidden in Slow Mail | core_tests; contract_tests | `CommandProcessor.Defer*`; `Contract.DeferFlagsOnly*` | automated | Assert message still in server Inbox with `flagged`; local queue excludes it. |
| A06 | App crashes during pending archive: command recovered, no false success, retry safe | core_tests; storage_tests; contract_tests | `CommandProcessor.RecoverInFlight*`; `SqliteStore.CommandSurvivesReopen*`; `Contract.CrashDuringArchive*` | automated | Simulate by persisting `InFlight`, reopening the DB, calling `recover_in_flight()`. Retry after the first move succeeded must end as Abandoned/Confirmed, never as an error shown as success (D21, ASM-17). |
| A07 | Network fails mid-delta: resume/retry, no cooldown until complete sync | core_tests; contract_tests | `SyncEngine.ResumeAfterNetworkError*`; `Contract.SyncInterruptedResumes*` | automated | Fake server drops the connection after page N; assert `resume_next_link` set, `last_complete_*` unchanged, second run resumes. |
| A08 | Another Outlook client moves a message: delta reconciliation updates the queue without resurrection | core_tests; contract_tests | `SyncEngine.RemovedIsGone*`; `Contract.MovedElsewhere*`; `Contract.ImmutableGapReconcile*` | automated | Includes the move-out-and-back "immutable gap" mode (R-DELTA-MOVEBACK); the reconcile requirement itself may still be "not yet" in code. |
| A09 | Device offline: cached mail readable, pending actions visible, eventual sync possible | core_tests | `TriageSession.OfflineQueues*`; `CommandProcessor.NetworkErrorKeepsPending*` | automated | Plus device item D-WIFI (manual). |
| A10 | Sign-out / revoke consent: tokens removed, user notified, cache handled per policy | core_tests; storage_tests; graph_tests | `TriageSession.SignOut*`; `SqliteStore.SecureWipe*`; `DeviceCodeAuth.SignOut*` | automated (local part); manual (server revoke) | Manual: in the synthetic tenant revoke the grant, then refresh; expect Auth error and a sign-in prompt (`invalid_grant`). Check that no token appears in logs: `grep -i bearer`. Also verify the wipe removes `-wal` and `-shm`. |
| A11 | HTML message with tracking image/script: no script or remote image execution/fetch | core_tests; contract_tests | `Sanitize.HtmlDropsScript*`; `Sanitize.HtmlDropsImg*`; `Contract.NoRemoteFetch*` | automated | Contract test asserts the fake server (and a second listener on another port) saw no request for the tracking URL. |
| A12 | Firmware/launcher incompatibility: safe failure and stock UI recoverable | tests/device | n/a | manual | Procedure in section 3.1. |
| A13 | Session ends/restarts: activity scoped correctly; deferred suppression persists | core_tests; storage_tests | `TriageSession.EndAndRestart*`; `TriageSession.DeferredSurvivesRestart*`; `SqliteStore.SessionItems*` | automated | |
| A14 | Huge inbox / long Unicode subjects: UI responsive, bounded memory, no truncated critical controls | core_tests; contract_tests; device | `Sanitize.LongUnicodeSubject*`; `Sanitize.Utf8TruncateBoundary*`; `Contract.Inbox1000*`; manual UI check | automated (data path); manual (UI) | Handover Stage 2 gate uses 1000 synthetic messages: `tools/fake_graph/server.py --generate 1000`. UI check: desktop simulator with long subjects (RTL, combining marks, emoji), then device checklist item D-UNICODE. |
| A15 | Permission denied / Conditional Access: clear actionable error, no insecure bypass | graph_tests; contract_tests; manual | `GraphBackend.MapHttpError403*`; `DeviceCodeAuth.ConditionalAccessBlock*`; `Contract.CABlockMode*` | automated (fake CA-block mode); manual (real tenant) | Manual procedure in section 3.2. |
| A16 | Clock changed backward/forward: cooldown cannot silently bypass or permanently lock the user | core_tests | `Cooldown.WallClockForwardWithinBoot*`; `Cooldown.WallClockBackward*`; `Cooldown.RebootUsesWall*`; `Cooldown.MonotonicBackwards*` | automated | Only **partly** satisfiable by design (D23): within a boot a wall change cannot bypass (monotonic); across a reboot a forward change bypasses and a backward change fails open. The tests document that behaviour; they do not claim A16 is met in full. |
| A17 | Email with PDF, DOCX and image attachments: list filename, type and size without fetching bytes; reading and actions still work | graph_tests; contract_tests | `GraphBackend.AttachmentsSelectOnly*`; `Contract.AttachmentsMetadata*` | automated | Fake server records any request for `contentBytes` or `$value` as a violation (GC 11.7). |
| A18 | Inline images and reference/item attachments: no automatic downloads; metadata handled gracefully; no broken UI | graph_tests; contract_tests | `GraphBackend.AttachmentTypes*`; `Contract.InlineAndReferenceAttachments*` | automated | Covers `@odata.type` with and without `#`, null `contentType`, reference-attachment `size` (GC 6.2). |
| A19 | Large or malicious attachment metadata: sanitised, bounded list, no unbounded allocation or external requests | core_tests; graph_tests | `Sanitize.AttachmentBounds*`; `Sanitize.AttachmentCountCap*`; `GraphBackend.OversizedResponse*` | automated | Caps: name 255, listed 100 (`Limits`); response cap 8 MiB in `CurlTransport`. |
| A20 | Optional PDF viewer spike | n/a | n/a | not yet (manual spike, post-MVP) | Procedure in section 3.3. Failure does not block the MVP (D11). |

Additional automated checks the plan expects (not in the handover matrix): redaction (`Log.Redact*`), session state machine legality (`SessionStateMachine.*`), SQLite schema migration (`SqliteStore.Migrate*`), WAL and 0600 mode (`SqliteStore.FileMode*`), device-code polling (`DeviceCodeAuth.Poll*` including `slow_down`), refresh-token rotation (`DeviceCodeAuth.RefreshRotates*`), throttle handling (`GraphBackend.RetryAfter*`, `Contract.Throttle*`), partial delta updates (`GraphBackend.PartialDeltaFollowUpGet*`, D22), delta expiry and resync (`SyncEngine.GoneResyncPrunes*`), and "no redirects followed".

## 3. Manual procedures

All device procedures need the device owner's go decision and a verified backup (D14). Do not run them against a corporate-connected device (THREAT_MODEL section 7).

### 3.1 A12 - Firmware or launcher incompatibility: safe failure, stock UI recoverable

Goal: show that when the launcher or OS does not work, the user ends up in the stock UI, and that our wrapper never leaves `xochitl` stopped.

Host dry run (no device), do this first:
1. Create a directory with a fake `systemctl` that appends its arguments to a log and exits 0, and a fake `slowmail` that exits 1. Put it first in `PATH`.
2. Run `packaging/run-slowmail.sh` with `SLOWMAIL_BIN` pointing at the fake binary.
3. Expect: the log shows `stop xochitl` then `start xochitl`, and the script exits non-zero without hanging. Repeat with the fake binary killed by `kill -TERM` and with the script itself sent SIGINT and SIGTERM; `start xochitl` must appear each time.

Device procedure (spare device, backup verified, OS and launcher versions recorded):
1. Record `cat /etc/os-release`, the launcher versions (xovi, AppLoad) and the OS build in TEST_LOG.md.
2. Take-over route: run the wrapper with a deliberately wrong binary path. Expect the stock UI to return within about a minute (passcode prompt expected, COMPAT 3 breakage 10) and `systemctl is-active xochitl` = `active`.
3. Run the wrapper and kill the app with `kill -9` from a second SSH session. Expect the stock UI back. Then `kill -9` the wrapper itself: expect `xochitl` to stay stopped (known limitation); recover with `systemctl start xochitl` over SSH or a reboot. Record this result honestly.
4. Launcher route (only if route B is being tried): record whether AppLoad appears and whether `xochitl` has `NRestarts=0` after 10 minutes. If the device crash-loops, reboot to stock and note it as a known breakage (COMPAT 3).
5. Pass if the stock UI was recoverable by documented steps in every case and no case needed more than a reboot.

### 3.2 A15 - Permission denied / Conditional Access: actionable error, no bypass

Needs a synthetic tenant and an admin account. Do not use a production tenant.
1. Register the app (public client, "Allow public client flows" on). Record the client id in the environment only.
2. Create a Conditional Access policy that blocks the device-code authentication flow for a test user, starting in report-only (GC 1.8). Sign in with the app; expect success and a report-only entry in the sign-in logs.
3. Switch the policy to enabled. Sign in again with a fresh install: expect the app to show a distinct "your admin blocks device-code sign-in" state, not a generic failure, and to offer no workaround.
4. With a session that was already signed in before step 3, wait for a refresh: expect `AADSTS530036` to surface as "sign in again", tokens cleared, and no retry loop (GC 1.8, 8 note 6).
5. Remove consent for `Mail.ReadWrite` (or use a user who cannot consent) and expect a 403 mapped to Forbidden with a "permission or licence issue" message and no retry.
6. Check that none of the messages shown or logged contain tokens, device codes or addresses.
7. Record the exact error strings and codes in ASSUMPTIONS.md (ASM-06) so the fake server can be corrected.

### 3.3 A20 - Optional PDF viewer spike (post-MVP, not scheduled)

Pass criteria must all be demonstrated independently of the MVP; failure is a valid outcome and is recorded in DECISIONS.md D11.
1. Feasibility: which PDF renderer exists on the device and in the SDK sysroot (Qt PDF is not listed in the sysroot; COMPAT toolchain facts)? Can the native document viewer be launched by a third-party app?
2. Security: validate MIME type against content, size cap, parse in a separate low-privilege process, safe temp file in a 0700 directory, delete after viewing, behaviour on a malformed or malicious PDF (use public test corpora only, on a throw-away device).
3. Performance: open time and page-turn latency for a 1 MB and a 10 MB PDF on the device.
4. Download is an explicit user action with a size cap, never automatic (invariant: no attachment bytes by default).
5. Re-run A17-A19 afterwards.

## 4. Stage gates (handover section 10) mapped to tests

| Stage | Gate | Where |
|---|---|---|
| 0 | Compatibility matrix, security go/no-go, SDK build on Linux x86_64, fake Graph fixture suite | COMPATIBILITY.md; THREAT_MODEL section 7; `contract_tests` |
| 1 | Launch/exit 20 times; reboot, stock UI still works; portrait text legible; no crash loop; measured input/refresh latency | tests/device/CHECKLIST.md |
| 2 | 1000-message dataset; first sync, delta, restart, network interruption, token expiry, server-side move/delete, duplicate suppression | A01, A07, A08, A14; `Contract.Inbox1000*` |
| 3 | Server confirms all operations; offline queue reconciles; retries do not duplicate; conflicts visible; nothing permanently deleted | A03-A06, A09 |
| 4 | 02:00 refresh allowed; 03:00 blocked; 06:00 eligible; cached mail always readable; outbound works at 03:00; failure at 02:00 does not start the timer | A01, A02, A03, A07, A16 |
| 5, 6 | Out of scope for the MVP | - |

## 5. Known gaps

- The fake server is our model of Graph (ASM-33). Open questions in GRAPH_CONTRACT section 12 can only be closed on a real tenant.
- No test covers power loss on real flash (R-SQLITE-POWERLOSS), E Ink rendering (R-EINK-REFRESH), or suspend/resume (R-SLEEP-SYNC) except by the device checklist.
- The Qt UI is exercised only by running the desktop simulator by hand; there are no automated UI tests tonight.
- Device cross-compile and install scripts are UNTESTED on hardware.
