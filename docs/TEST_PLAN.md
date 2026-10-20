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

**Naming and status caveat.** The test names below were reconciled with the test sources present in `tests/unit/` on 2026-10-20 (read from the `TEST(Suite, Name)` declarations; **not yet built or run by the documentation agent**). The graph adapter tests all live in the GoogleTest fixture `Fixture`, the auth tests in `AuthFixture`. `tests/contract/` contained only fixtures (`inbox_small.json`, `inbox_1000.json`) and no `*.cpp` at that time, and there are no `TriageSession` unit tests yet; rows that depend on those say "contract: not yet" or "not yet". The integrator must re-run `--gtest_list_tests` and update this table and TEST_LOG.md; a row is "automated" only if the named test exists and passes.

Fixtures: `tests/contract/fixtures/` (path compiled into `contract_tests` as `SLOWMAIL_FIXTURE_DIR`). The meta-tests of GRAPH_CONTRACT 11.9 (immutable header on every request, nextLink never rewritten, no `contentBytes` / `$value`, concurrency bound, `Retry-After` honoured, resync prunes, tolerant parsing, no secrets in logs, refresh-token rotation) apply to every contract scenario.

## 2. Acceptance matrix A01-A20 (handover section 11)

Status: **automated** (a named unit test exists in the tree; pass/fail is recorded in TEST_LOG.md), **partly** (automated for some of the expected behaviour, rest listed), **manual** (procedure in this document or the device checklist), **not yet** (no test and no procedure ready to run). "Contract" tests are planned in `tests/contract/` against the fake server and did not exist when this was written.

| ID | Scenario | Target | Test names (`Suite.Test`) | Status | Notes / manual procedure |
|---|---|---|---|---|---|
| A01 | First eligible manual sync at 02:00: inbox fetched, complete delta saved, next eligibility 06:00 | core_tests | `SyncEngine.FirstSyncRecordsSuccessAndStartsCooldown`; `Cooldown.FirstDownloadIsEligible`; `Cooldown.StageFourGate_0200_0300_0600`; `Cooldown.RecordSuccessFillsAllFields` | automated (unit); contract: not yet | Contract test to add: `Contract.FirstSync*` against the fake server (delta link committed only after the last page). |
| A02 | Refresh at 03:00: no inbound Graph fetch, cached queue available | core_tests | `SyncEngine.RefreshAtPlusOneHourIsPolicyErrorAndQueueStaysReadable`; `Cooldown.StageFourGate_0200_0300_0600` | automated (unit); contract: not yet | Contract test to add: fake server request log shows zero delta calls. |
| A03 | Archive at 03:00: Graph move succeeds despite cooldown, original leaves Inbox | core_tests; graph_tests | `CommandProcessor.ArchiveConfirmsDespiteInboundCooldown`; `Fixture.MoveSendsDestinationIdAndParsesResult`; `Fixture.MoveFolderNotFoundIsDistinguishableFromItemNotFound`; `CommandProcessor.NotFoundWithOtherCodeIsNonRetryableFailureNotGone` | automated (unit); contract: not yet | Missing `archive` folder must fail visibly, never fall back to `deleteditems` (D4, ASM-01). The code `ErrorFolderNotFound` is our assumption (ASM-15); a contract test with the fake server in "no archive folder" mode and, later, the synthetic tenant is needed. |
| A04 | Delete at 03:00: message in Deleted Items, not permanently removed | core_tests; graph_tests | `CommandProcessor.DeleteMovesToDeletedItemsNotHardDelete`; `Fixture.MoveToDeletedItemsUsesWellKnownName` | automated (unit); contract: not yet | Contract: assert no permanent-delete call and the message sits in `deleteditems`. |
| A05 | Defer at 03:00: flag set, original remains in Inbox, hidden in Slow Mail | core_tests; graph_tests | `CommandProcessor.DeferFlagsOnServerKeepsInboxCopyAndHidesLocally`; `Fixture.SetFlagSendsPatchBody`; `CommandProcessor.UndeferClearsFlagAndRestoresQueue` | automated (unit); contract: not yet | |
| A06 | App crashes during pending archive: command recovered, no false success, retry safe | core_tests; storage_tests | `CommandProcessor.InFlightIsPersistedBeforeTheNetworkCall`; `CommandProcessor.CrashDuringInFlightIsRecoveredAndRetried`; `CommandProcessor.CrashRecoveryWhenServerAlreadyActedIsAbandonedNotDuplicated`; `SqliteStoreFile.ReopenKeepsDataAcrossEveryTable`; `SqliteStoreFile.UncommittedTransactionIsNotDurable` | automated | Retry after the first move already succeeded depends on ASM-17 (real Graph behaviour). |
| A07 | Network fails mid-delta: resume/retry, no cooldown until complete sync | core_tests | `SyncEngine.ErrorOnPageTwoOfThreeResumesAndDoesNotConsumeCooldown`; `SyncEngine.ErrorWhenStartingFromDeltaLinkLeavesDeltaLinkAndNoResume`; `Cooldown.FailedSyncDoesNotStartTimer`; `Cooldown.FailedSyncAfterSuccessKeepsOriginalTimer`; `SyncEngine.StorageFailureMidPageRollsBackAndRetrySucceeds` | automated (unit); contract: not yet | Contract: fake server drops the connection after page N. |
| A08 | Another Outlook client moves a message: delta reconciliation updates the queue without resurrection | core_tests; graph_tests | `SyncEngine.AnotherClientMovedMessageWithoutCommandLeavesQueue`; `SyncEngine.AnotherClientMovedMessageWithPendingCommandAbandonsCommand`; `SyncEngine.MovedOutThenBackUnTombstones`; `SyncEngine.GoneTriggersFullResyncAndTombstonesUnseenMessages`; `SyncEngine.ReplayingTheSamePageTwiceYieldsSameStoreState`; `Fixture.ParseDeltaPageWithNextLinkAndRemoved` | partly | `MovedOutThenBackUnTombstones` covers a returning message that *is* reported. The known Graph gap where the return is **not** reported (R-DELTA-MOVEBACK) needs the fake server's "immutable gap" mode and a periodic-reconcile test: not yet. |
| A09 | Device offline: cached mail readable, pending actions visible, eventual sync possible | core_tests | `CommandProcessor.NetworkErrorLeavesPendingStopsAndSucceedsLater`; `SyncEngine.RefreshAtPlusOneHourIsPolicyErrorAndQueueStaysReadable` | partly | `TriageSession` offline behaviour (`set_online(false)`, Pending indicator in `StatusSnapshot`) has no test yet. Device item D-WIFI (manual). |
| A10 | Sign-out / revoke consent: tokens removed, user notified, cache handled per policy | core_tests; storage_tests; graph_tests | `AuthFixture.SignOutClearsStoreAndCache`; `AuthFixture.InvalidGrantClearsBlobAndReturnsAuth`; `AuthFixture.InteractionRequiredAndFlowCheckAlsoClear`; `AuthFixture.ErrorsNeverContainSecrets`; `SqliteStoreFile.SecureWipeRemovesAllFiles`; `SqliteStoreFile.SecureWipeLeavesNoPlaintextInFile`; `MemoryStore.SecureWipeClearsEverything` | partly; manual (server-side revoke) | `TriageSession::sign_out` itself has no test yet. Manual: in a synthetic tenant revoke the grant, then refresh; expect an Auth error and a sign-in prompt (`invalid_grant`). Check logs for tokens: `grep -i bearer`. |
| A11 | HTML message with tracking image/script: no script or remote image execution/fetch | core_tests | `HtmlToText.ScriptStyleHeadAndFriendsDropped`; `HtmlToText.TrackingPixelRemovedNoRemoteRefs`; `HtmlToText.DangerousLinksDropped`; `HtmlToText.LinkUrlsAreNeutralised`; `HtmlToText.EntityDecodedMarkupIsNotReparsed`; `HtmlToText.PathologicalInputsAreLinearAndSafe`; `HtmlToText.EveryPrefixOfADocumentIsSafe` | automated (sanitiser); contract: not yet | Contract test to add: a second listener sees no request for the tracking URL. The UI never loads HTML at all (no web view). |
| A12 | Firmware/launcher incompatibility: safe failure and stock UI recoverable | tests/device | n/a | manual | Procedure in section 3.1. |
| A13 | Session ends/restarts: activity scoped correctly; deferred suppression persists | core_tests; storage_tests | `SessionMachine.*`; `MemoryStore.SessionsAndItems`; `SqliteStoreFile.ReopenKeepsDataAcrossEveryTable` | partly | `TriageSession.start/end` resume logic and "deferred suppression persists across restart" have no dedicated test yet: not yet. |
| A14 | Huge inbox / long Unicode subjects: UI responsive, bounded memory, no truncated critical controls | core_tests; graph_tests; manual | `SanitizeLine.CapIsUtf8SafeAndTrimmed`; `Utf8Truncate.Boundaries`; `HtmlToText.TruncationOnUtf8Boundary`; `HtmlToText.OneMegabyteInputIsFast`; `Fixture.FetchBodyCapsHugeBody`; `Fixture.ParseMessageSummaryToleratesMissingFieldsAndSanitises` | partly; manual (UI) | 1000-message sync (Stage 2 gate) needs `Contract.Inbox1000*` (fixture `tests/contract/fixtures/inbox_1000.json` exists; test not yet). UI: desktop simulator with long, RTL, combining-mark and emoji subjects; device item D-UNICODE. |
| A15 | Permission denied / Conditional Access: clear actionable error, no insecure bypass | graph_tests; manual | `Fixture.MapHttpErrorTable`; `Fixture.SecondUnauthorizedIsAuthError`; `Fixture.Unauthorized401InvalidatesAndRetriesOnce`; `AuthFixture.InteractionRequiredAndFlowCheckAlsoClear`; `CommandProcessor.ForbiddenFailsImmediately`; `CommandProcessor.AuthErrorLeavesPendingAndSignalsAuthRequired` | partly; manual (real tenant) | Fake-server CA-block mode and the UI wording are not yet. Manual procedure in section 3.2. |
| A16 | Clock changed backward/forward: cooldown cannot silently bypass or permanently lock the user | core_tests | `Cooldown.WallClockMovedBackwardsOnSameBootUsesMono`; `Cooldown.MonoStillUsedWithinBootWhenWallJumpsForward`; `Cooldown.WallClockMovedBackwardsAcrossRebootIsEligibleAnomaly`; `Cooldown.WallForwardFourHoursAcrossRebootIsEligible`; `Cooldown.WallForwardLessThanFourHoursAcrossRebootBlocked`; `Cooldown.MissingMonoFallsBackToWall` | automated | Only **partly** satisfiable by design (D23): within a boot a wall-clock change cannot bypass; across a reboot a forward change bypasses and a backward change fails open (R-COOLDOWN-CLOCK). The tests document that behaviour; they do not show A16 is met in full. |
| A17 | Email with PDF, DOCX and image attachments: list filename, type and size without fetching bytes; reading and actions still work | graph_tests | `Fixture.FetchBodyConvertsHtmlAndClassifiesAttachments`; `Fixture.FetchBodyPlainTextIsSanitisedAndNoAttachmentCallWhenNone` | partly | Assert the attachment request uses `$select` without `contentBytes`; the fake-server "violation" recorder (GC 11.7) is a contract test: not yet. |
| A18 | Inline images and reference/item attachments: no automatic downloads; metadata handled gracefully; no broken UI | graph_tests; core_tests | `Fixture.FetchBodyListsAttachmentsForInlineCidMarkerEvenIfFlagFalse`; `Fixture.AttachmentListForbiddenOrMissingIsTolerated`; `Fixture.AttachmentListServerErrorPropagates`; `SanitizeAttachment.NegativeAndUnknownSizes` | partly | Reference and item attachment kinds are covered inside `FetchBodyConvertsHtmlAndClassifiesAttachments`; verify when run. |
| A19 | Large or malicious attachment metadata: sanitised, bounded list, no unbounded allocation or external requests | core_tests; graph_tests | `SanitizeAttachment.HugeNameIsBoundedAndSafe`; `SanitizeAttachment.HugeNameCutInsideMultibyteChar`; `SanitizeAttachment.ListIsBounded`; `SanitizeAttachment.ControlCharsInContentTypeAndName`; `SanitizeAttachment.EmptyNameGetsPlaceholder`; `Fixture.FetchBodyCapsHugeBody` | automated | Caps: name 255, listed 100 (`Limits`); transport response cap 8 MiB (no unit test named for it yet). |
| A20 | Optional PDF viewer spike | n/a | n/a | not yet (manual spike, post-MVP) | Procedure in section 3.3. Failure does not block the MVP (D11). |

Other tests present in the tree that support design decisions: D21 `CommandProcessor.NotFoundWithItemNotFoundOrEmptyCodeMeansGone`, `CommandProcessor.NotFoundWithOtherCodeIsNonRetryableFailureNotGone`; D22 `Fixture.PartialDeltaItemIsReplacedByFullFetch`, `Fixture.PartialDeltaItemThatVanishedBecomesRemoved`, `Fixture.PartialFollowUpOtherErrorsPropagate`; D20 `SyncEngine.UnflaggedElsewhereRestoresDeferredMessageWithUndeferredEvent`; throttling `Fixture.Throttle429WithRetryAfterIsRetried`, `Fixture.Throttle429WithoutRetryAfterBacksOffThenReportsNoRetryAfter`, `Fixture.Html503WithoutRetryAfterIsServerError`, `CommandProcessor.ThrottledWithRetryAfterStaysPendingWithNotBeforeAndStops`; immutable ids `Fixture.ImmutableIdPreferIsSentOnEveryRequestKind`; no leakage of the token to another host `Fixture.LinkToForeignOriginIsRefusedWithoutSendingToken`; resync `Fixture.Delta410IsGone`, `Fixture.DeltaSyncStateCodeOn4xxIsGone`; redaction `Log.Redact*`; SQLite mode and journal `SqliteStoreFile.FileModeIs0600`, `SqliteStoreFile.UsesWalAndFullSynchronousAndForeignKeys`; migrations `Migration.*`; device-code polling `AuthFixture.PollStateMachine`, `AuthFixture.ExpiredTokenIsRefreshedAndRotatedRefreshTokenStored`.

Not covered by any test yet: "no redirects followed" and TLS-verification options in `CurlTransport` (no unit test named for them; verify by reading `curl_transport.cpp` or add a contract test), the `Prefer` comma-joined versus separate header question (ASM-14), and `TriageSession` as a whole.

## 3. Manual procedures

All device procedures need the device owner's go decision and a verified backup (D14). Do not run them against a corporate-connected device (THREAT_MODEL section 7).

### 3.1 A12 - Firmware or launcher incompatibility: safe failure, stock UI recoverable

Goal: show that when the launcher or OS does not work, the user ends up in the stock UI, and that our wrapper never leaves `xochitl` stopped.

Host dry run (no device), do this first:
1. Create a directory with a fake `systemctl` that appends its arguments to a log and exits 0, and a fake `slowmail` that exits 1. Put it first in `PATH`.
2. Run `sh packaging/run-slowmail.sh` with `SLOWMAIL_BIN` pointing at the fake binary and `SLOWMAIL_ENV_FILE=/nonexistent`. (The documentation agent ran this dry run once on 2026-10-20 with a logging `systemctl` shim: exit status of the app was propagated and `mask`, `stop xochitl`, `unmask`, `start xochitl` were logged in that order, also after SIGTERM; a missing binary exited 127 without calling `systemctl`. Not recorded in TEST_LOG.md; re-run it there.)
3. Expect: the log shows `stop xochitl` then `start xochitl`, and the script exits non-zero without hanging. Repeat with the fake binary killed by `kill -TERM` and with the script itself sent SIGINT and SIGTERM; `start xochitl` must appear each time.

Device procedure (spare device, backup verified, OS and launcher versions recorded):
1. Record `cat /etc/os-release`, the launcher versions (xovi, AppLoad) and the OS build in TEST_LOG.md.
2. Take-over route: (a) run the wrapper with a wrong `SLOWMAIL_BIN` path: it must refuse (exit 127) without stopping `xochitl`. (b) Point `SLOWMAIL_BIN` at a shell script that exits with status 1: the stock UI must return within about a minute (passcode prompt expected, COMPATIBILITY breakage 10) and `systemctl is-active xochitl` must print `active`.
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
| 2 | 1000-message dataset; first sync, delta, restart, network interruption, token expiry, server-side move/delete, duplicate suppression | A01, A07, A08, A14; `Contract.Inbox1000*` (not yet written) |
| 3 | Server confirms all operations; offline queue reconciles; retries do not duplicate; conflicts visible; nothing permanently deleted | A03-A06, A09 |
| 4 | 02:00 refresh allowed; 03:00 blocked; 06:00 eligible; cached mail always readable; outbound works at 03:00; failure at 02:00 does not start the timer | A01, A02, A03, A07, A16 |
| 5, 6 | Out of scope for the MVP | - |

## 5. Known gaps

- The fake server is our model of Graph (ASM-33). Open questions in GRAPH_CONTRACT section 12 can only be closed on a real tenant.
- No test covers power loss on real flash (R-SQLITE-POWERLOSS), E Ink rendering (R-EINK-REFRESH), or suspend/resume (R-SLEEP-SYNC) except by the device checklist.
- The Qt UI is exercised only by running the desktop simulator by hand; there are no automated UI tests tonight.
- Device cross-compile and install scripts are UNTESTED on hardware.
