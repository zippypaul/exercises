# Slow Mail - Decision Log

Status: pre-alpha. Last updated: 2026-10-20.

All decisions below were taken by the product owner on 2026-10-20. D1-D16 correspond one-to-one to the handover's decision register Q1-Q16 (handover section 3). For each of them the handover's column "Recommendation / default for spike" was adopted as the decision. D17-D24 are engineering decisions recorded alongside them.

How to read the evidence tags: VERIFIED / UNVERIFIED / FALSIFIED have the meaning defined in [GRAPH_CONTRACT.md](GRAPH_CONTRACT.md) section 0 and [COMPATIBILITY.md](COMPATIBILITY.md). A decision can be adopted while the facts it rests on are still UNVERIFIED; the "How to revisit" line says what evidence would change it. The assumptions behind these decisions are numbered in [ASSUMPTIONS.md](ASSUMPTIONS.md) (ASM-nn).

## Summary table

| ID | Handover ref | Decision (short) | Owner | Status |
|---|---|---|---|---|
| D1 | Q1 | Test tenant first; tenant-admin approval before any real mailbox | Owner / IT admin | Adopted |
| D2 | Q2 | Device-code sign-in, only if tenant policy allows it | Engineering + IT | Adopted (see D18) |
| D3 | Q3 | `Do` = explicit Archive, no reply in MVP | Product owner | Adopted |
| D4 | Q4 | Archive = the `archive` well-known folder; never the Online Archive mailbox | Engineering | Adopted |
| D5 | Q5 | Defer persists while flagged; deferred shown as a count only | Product owner | Adopted (see D20) |
| D6 | Q6 | Pre-existing flagged messages are NOT hidden | Product owner | Adopted |
| D7 | Q7 | Session ends by explicit end; state survives a crash | Product owner | Adopted |
| D8 | Q8 | Offline actions are queued locally with a Pending indicator | Product owner | Adopted |
| D9 | Q9 | Cooldown: sign-in eligible, failed refresh free, complete sync starts clock | Product owner | Adopted (see D23) |
| D10 | Q10 | No new inbound retrieval until the next eligible manual refresh | Product owner | Adopted |
| D11 | Q11 | Attachments listed (name, size, type) only; sanitised text body; no PDF viewer | Engineering / owner | Adopted |
| D12 | Q12 | Local session journal for sent/deleted/deferred; no folder sync | Product owner | Adopted |
| D13 | Q13 | No production mailbox until threat model approved; no encryption claims | Security owner | Adopted |
| D14 | Q14 | Explicit go/no-go and verified backup before touching the device | Device owner | Adopted |
| D15 | Q15 | Record firmware / SDK / launcher versions; compatibility spike first | Engineering | Adopted |
| D16 | Q16 | Keep licences separate; no licensing or paywall code in the MVP | Product owner | Adopted |
| D17 | - | C++17 core kept Qt-free | Engineering | Adopted |
| D18 | - | Device-code flow over auth-code + PKCE | Engineering + IT | Adopted |
| D19 | - | Delete = move to `deleteditems` | Engineering | Adopted |
| D20 | - | Defer = follow-up flag + local suppression; unflag elsewhere restores to queue | Engineering | Adopted |
| D21 | - | NotFound: `ErrorItemNotFound` => already gone, never retried; other 404 codes => failed | Engineering | Adopted |
| D22 | - | Partial delta updates are resolved in the Graph adapter by a follow-up GET | Engineering | Adopted |
| D23 | - | Cooldown clock: monotonic within a boot, wall across boots, anomalies fail open | Engineering | Adopted |
| D24 | - | Device launch: MVP targets takeover (route A) for the Stage 1 gate; AppLoad native QML (route B) is the likely long-term path | Engineering | Adopted |

Tonight's scope (2026-10-20), also decided by the owner: no pull request, no monetisation code, no device modification. Build target is the Qt-free C++17 core, SQLite storage, Graph adapter, Python fake Graph server, contract tests and a Qt 6.4 Quick UI compiled on the Ubuntu host (desktop simulator). The device cross-compile is scripted but UNTESTED on hardware, because this environment has no SDK and cannot reach developer.remarkable.com.

---

## D1 (Q1) - Entra tenant and consent

- **Decision.** Validate against a test tenant first. Obtain tenant-admin approval before any real work mailbox is used. The Entra app registration is made by the owner later; the client id and tenant id are read from `SLOWMAIL_CLIENT_ID` and `SLOWMAIL_TENANT_ID` and are never committed.
- **Rationale.** A public-client app with delegated `Mail.ReadWrite` needs "Allow public client flows" enabled (GRAPH_CONTRACT 1.7, VERIFIED). Whether user consent suffices or admin consent is required is tenant policy (UNVERIFIED, GRAPH_CONTRACT 1.7 and 12.10).
- **Consequences.** Nothing in the repository can sign in until the owner supplies a client id. All tonight's testing uses the fake server. A tenant that refuses public-client apps ends the native approach; the handover's companion-bridge alternative (section 4) would then have to be evaluated, and it adds a trust boundary.
- **How to revisit.** After the owner registers the app and a synthetic-tenant sign-in is attempted. Record the result in ASSUMPTIONS.md (ASM-06, ASM-07).

## D2 (Q2) - First sign-in on a tablet without a browser

- **Decision.** OAuth device-code flow, used only if tenant and security policy allow it, with the second step done on a phone or computer.
- **Rationale.** The Paper Pro has no conventional browser (handover section 3, Q2). See D18 for why PKCE was not chosen.
- **Consequences.** Sign-in depends on Conditional Access not blocking device code (RISKS R-CA-DEVICECODE). Even after a successful sign-in, later refreshes can be blocked (`AADSTS530036`, GRAPH_CONTRACT 1.8), which means the user is signed out and the app must show a clear message.
- **How to revisit.** If the tenant blocks device code, this is a conversation with the tenant admin (D18), not something to bypass in code.

## D3 (Q3) - What `Do` means

- **Decision.** `Do` is an explicit `Done / Archive` action. There is no reply composer in the MVP. The app never marks something done merely because it was viewed.
- **Rationale.** Reply requires `Mail.Send`, handwriting/pen input and a send/archive partial-failure model (handover Stage 5). None of that is in scope.
- **Consequences.** `Mail.Send` is not requested. README lists it as a later scope.
- **How to revisit.** Stage 5 of the handover plan. Adding `Mail.Send` needs a new interactive sign-in (GRAPH_CONTRACT 1.7).

## D4 (Q4) - What "archive" is

- **Decision.** Archive = move to the Outlook `archive` well-known folder. Existence and move semantics are to be verified per tenant. Never confuse it with the Exchange Online Archive mailbox. If the folder is absent, the app must say so and must not silently fall back to Deleted Items (GRAPH_CONTRACT 4, implementation note 2).
- **Rationale.** `archive` is a documented well-known name, but it is the One-Click Archive folder and "isn't the same as the Archive Mailbox feature of Exchange online" (GRAPH_CONTRACT 4.3, VERIFIED). Graph does not reliably reach the Online Archive mailbox (SNIPPET).
- **Consequences.** In a mailbox without that folder, Do fails visibly. A message moved to an Online Archive mailbox would get a new id (GRAPH_CONTRACT 3.2); Slow Mail cannot do that. Note the open design point: `core::MailBackend::move` takes a `WellKnownFolder` and the adapter header sends the literal name (`"archive"`), whereas GRAPH_CONTRACT recommends resolving ids with `GET /me/mailFolders/archive` first. See the contradiction list in the hand-off report; the behaviour for a missing folder must be covered by a contract test (TEST_PLAN A03).
- **How to revisit.** After the first test-tenant run (ASM-01).

## D5 (Q5) - Does defer persist?

- **Decision.** Defer persists while the message stays flagged in Outlook. The UI shows a deferred count; an optional filtered "Deferred" list exists only if the user turns it on (`TriageConfig::show_deferred_panel`, default false).
- **Rationale.** The user wants an Inbox-only experience (handover Q5).
- **Consequences.** A deferred message does not return by itself. It returns when someone clears the flag elsewhere (D20) or when the user explicitly un-defers it. There is no snooze date in the MVP.
- **How to revisit.** If the user wants resurfacing by date, add a start/due date to the flag (GRAPH_CONTRACT 5.2 shows the shape; both fields are ILLUSTRATIVE there).

## D6 (Q6) - Pre-existing flagged messages

- **Decision.** Do not hide them. Only messages deferred by Slow Mail are suppressed locally (`TriageConfig::hide_preexisting_flagged` default false).
- **Rationale.** Hiding mail the user flagged for other reasons would be a surprising loss of visibility.
- **Consequences.** A message flagged in Outlook before Slow Mail saw it shows in the queue. This depends on flag state arriving in delta (ASM-11).
- **How to revisit.** If the user asks for it, flip the config default; no schema change needed.

## D7 (Q7) - End of session

- **Decision.** A session ends by an explicit end/close action. Session state is persisted so that a crash resumes the same session.
- **Rationale.** Timeouts would end a session while the user is thinking; the E Ink use case is slow.
- **Consequences.** `TriageSession::start()` resumes the active session if one exists. Pending commands survive into the next session. Activity is scoped to a session (A13).
- **How to revisit.** If stale sessions accumulate, add an idle auto-end, but only as an explicit owner decision.

## D8 (Q8) - Offline actions

- **Decision.** When offline, archive/delete/defer are accepted, queued durably with a visible Pending indicator, and sent on the next connection. This is independent of the four-hour inbound cooldown.
- **Rationale.** Handover invariants 2 and 3: mutations are not rate-limited by inbound refresh, and an action is never reported as done until Graph confirms.
- **Consequences.** A queued action can meet a changed server state later (message moved or deleted elsewhere): see D21. Pending commands are lost only by secure wipe, which is intended.
- **How to revisit.** If queue growth or stale commands become a problem, add an age limit with explicit user confirmation.

## D9 (Q9) - What the cooldown applies to

- **Decision.** Initial sign-in is eligible for a download. A failed or partial refresh does not consume the cooldown. Only a successful, complete sync starts the clock. Token-refresh-only operations do not consume it. Pro entitlement lifts it (behind a flag, no payment code). Clock changes are handled by D23.
- **Rationale.** Handover section 6.
- **Consequences.** Reinstalling or secure-wiping removes `last_complete_*` and therefore makes the first download eligible again; this is a known and accepted bypass for a personal MVP (RISKS R-COOLDOWN-CLOCK). `SLOWMAIL_PRO=1` lifts the cooldown for testing.
- **How to revisit.** If the cooldown is ever sold or enforced, it needs server-side time and an entitlement store, which contradicts "no backend". Out of scope.

## D10 (Q10) - New mail during an open session

- **Decision.** No new inbound retrieval until the next eligible manual refresh.
- **Rationale.** No push, no background polling (handover section 1).
- **Consequences.** Fetching the body or attachment metadata of a message that is already in the queue is allowed at any time, because it is one message on demand and not new mail (`TriageSession::open`). This reading of "inbound" is an interpretation by engineering; the handover does not spell it out. Flagged for the owner to confirm.
- **How to revisit.** If the owner reads "no inbound retrieval" as covering body fetches too, then bodies would have to be fetched during the refresh, which raises bandwidth and storage.

## D11 (Q11) - Attachments and HTML

- **Decision.** List attachments with filename, human-readable size and type. Do not download, open or save them. The message body is shown as sanitised plain text. A PDF viewer is a post-MVP feasibility spike (A20) and is not attempted tonight.
- **Rationale.** The owner's decision and addendum of 2026-10-21 (attachments are not a core use case). Not attempting PDF viewing avoids a document-parser attack surface on a device with weakened secure boot.
- **Consequences.** The adapter must use `$select=id,name,contentType,size,isInline` on the attachments call, because the default listing includes `contentBytes` (GRAPH_CONTRACT 6.2). Whether `$select` is honoured on that collection is UNVERIFIED (ASM-12). Reason PDF viewing is omitted: it is not straightforward (native viewer integration, MIME validation, safe temp storage and malicious-PDF handling are all unresearched), so by the owner's rule it is omitted and the reason recorded here.
- **How to revisit.** A separate spike with its own security review.

## D12 (Q12) - Session journal

- **Decision.** A local session journal records what the user did (archived, deleted, deferred; later sent, delegated) with Pending / Confirmed / Failed. Subject and sender are captured as a snapshot at action time. No Sent Items or Deleted Items folder is read.
- **Rationale.** The activity view must work without a general folder browser (handover section 8).
- **Consequences.** The snapshot contains subject and sender, which are sensitive (THREAT_MODEL). It is stored in the same database as the cache and is removed by secure wipe.
- **How to revisit.** If the journal should be shorter-lived, add a retention period.

## D13 (Q13) - Encryption and credential storage

- **Decision.** No production mailbox until the threat model is approved. Tokens are stored as an opaque blob in the local SQLite database (file mode 0600). Encryption at rest is NOT implemented, and Slow Mail does not claim a secure enclave or encrypted database anywhere.
- **Rationale.** COMPATIBILITY section 5 found no evidence of a keyring, Secret Service or TPM API on the device OS; existing community apps keep secrets in plaintext. `/home` sits on an encrypted volume, so at-rest protection comes from device encryption only, and developer mode weakens secure boot (COMPATIBILITY section 4).
- **Consequences.** Anyone with root access to a developer-mode device can read tokens and cached mail. See THREAT_MODEL and RISKS R-TOKEN-AT-REST. GRAPH_CONTRACT section 1 note 4 says to "persist the refresh token in the OS credential store"; that is not possible on the device as far as is known, so this decision overrides it.
- **How to revisit.** If a keystore is found on the device (COMPATIBILITY open item 10), or if SQLCipher-style encryption becomes practical and licence-compatible. Encrypting with a key stored next to the data adds little and is not planned.

## D14 (Q14) - Factory reset and unsupported installation

- **Decision.** Explicit go/no-go by the device owner and a verified backup before any device change. Nothing on a device is touched tonight, and `scripts/install-device.sh` refuses to run without `SLOWMAIL_I_HAVE_A_BACKUP=1`.
- **Rationale.** Enabling developer mode factory-resets the device and weakens secure boot (COMPATIBILITY section 4).
- **Consequences.** Device testing waits for the owner. The device scripts are UNTESTED.
- **How to revisit.** At the Stage 1 gate (tests/device/CHECKLIST.md).

## D15 (Q15) - Firmware, SDK and launcher versions

- **Decision.** Record model, OS build, SDK version and launcher versions, and run the compatibility spike before building features on the device path.
- **Rationale.** The community evidence is version-specific and partly contradictory (COMPATIBILITY section 3: AppLoad 0.5.3 crash-loops on OS 3.28).
- **Consequences.** COMPATIBILITY.md is desk research only; nothing in it was tested on hardware. The first device session fills the table in TEST_LOG.md.
- **How to revisit.** Whenever the OS, SDK or launcher version changes.

## D16 (Q16) - Selling the product later

- **Decision.** Keep dependency licences and distribution separate. No licensing or paywall code in the MVP.
- **Rationale.** Handover Q16 and Stage 6.
- **Consequences.** [LICENSES.md](LICENSES.md) lists dependencies. Two items need a legal read before any distribution: the reMarkable SDK EULA, and AppLoad's GPL-3.0 QML module if the app uses it as a native-QML AppLoad frontend (COMPATIBILITY section 2, licence note, UNVERIFIED legal inference).
- **How to revisit.** Before Stage 6.

## D17 - C++17 core kept Qt-free

- **Decision.** `src/core`, `src/storage` and `src/graph` are plain C++17 and do not link Qt. Only `src/ui` uses Qt.
- **Rationale.** The core has to build and test on the Ubuntu host (Qt 6.4) and be cross-compiled for the device (SDK Qt 6.8.2 per COMPATIBILITY, device Qt 6.10.3 on OS 3.28). Keeping Qt out of the core means the version gap affects only the UI, makes the tests run without a display, and lets the CLI exercise the whole stack headless. It also keeps the LGPL obligations confined to the UI binary.
- **Consequences.** Some things Qt would give for free (HTTP, SQL, JSON) are done with libcurl, SQLite and nlohmann/json. The SDK sysroot has no Qt Sql module (COMPATIBILITY toolchain facts), so using plain SQLite is also a device requirement. A thin bridge in `src/ui` runs the synchronous core calls off the UI thread.
- **How to revisit.** Only if a Qt-only facility becomes necessary (for example secure storage); then add it behind an interface in `src/platform`.

## D18 - Device-code flow chosen over auth-code + PKCE

- **Decision.** The tablet has no browser, so the app signs in with the OAuth device authorization grant. Microsoft recommends blocking device code flow in Conditional Access ("a high-risk authentication method"; GRAPH_CONTRACT 1.8, VERIFIED, ms.date 2026-03-24), so whether this works in a given tenant is a tenant-admin conversation, not something the app can negotiate around.
- **Rationale.** Auth-code + PKCE needs a browser on the device, or a loopback redirect on the same machine. GRAPH_CONTRACT section 1 note 1 suggests offering both; the owner chose device code only for the MVP.
- **Consequences.** (1) The app registration must have "Allow public client flows" on. (2) Hardened tenants may refuse sign-in, or may break refreshes later through protocol tracking (`AADSTS530036`). (3) The app must show a distinct, actionable "your admin blocks device-code sign-in" state and must not offer an insecure bypass (A15). (4) `slow_down` is not in Microsoft's documented error table (VERIFIED absence); the client handles it defensively per RFC 8628.
- **How to revisit.** If the tenant admin blocks device code and will not make an exception, implement PKCE with a companion device, or reconsider the architecture (handover section 4 fallback).

## D19 - Delete = move to `deleteditems`

- **Decision.** Delete moves the message to the `deleteditems` well-known folder. The app never calls `permanentDelete` and never hard-deletes.
- **Rationale.** Handover section 1 and invariant 6. Recoverable by the user in Outlook.
- **Consequences.** "Deleted" in the session journal means "moved to Deleted Items and confirmed by Graph". The message then leaves the Inbox delta as a removal (GRAPH_CONTRACT 2.5). Retention in Deleted Items is the tenant's policy and is not controlled by Slow Mail.
- **How to revisit.** Not planned. Permanent delete would be a separate, explicitly confirmed feature.

## D20 - Defer = follow-up flag + local suppression; unflag elsewhere restores to queue

- **Decision.** Defer sets `flag.flagStatus = flagged` through `PATCH /me/messages/{id}`, leaves the message in the Inbox, and hides it from the Slow Mail queue by local state. If a later delta shows the message with the flag cleared (someone un-flagged it in Outlook), the sync engine restores it to the queue and records an "Undeferred" event.
- **Rationale.** Handover Q5/Q6 and workflow section 5. Restoring on unflag gives the user a way to bring a message back using the tool they already have (Outlook), and keeps Outlook as the source of truth (invariant 8).
- **Consequences.** This relies on flag state being selectable in delta and on flag changes producing a delta entry. GRAPH_CONTRACT 5.2 note 7 and 12.4 say this is UNVERIFIED (ASM-11). If it is false, deferred items only come back through an explicit un-defer in Slow Mail. A message marked `complete` in Outlook is neither `flagged` nor cleared; how that is treated is an open detail for the contract tests.
- **How to revisit.** After the first real-tenant run of the flag contract test.

## D21 - NotFound handling

- **Decision.** A Graph 404 with code `ErrorItemNotFound` means "the message is already gone from where we expected it". The command is abandoned, the message is marked gone locally, the event shows "already gone", and the command is never retried. Any other 404 code is a failure (the command is marked Failed with a reason), not "already gone".
- **Rationale.** A queued command must be safe to retry after an unknown outcome and after a crash (A06). With immutable ids a 404 on a message most likely means it was deleted or moved by another client (GRAPH_CONTRACT 8.5; the code string `ErrorItemNotFound` is SNIPPET-grade, scenario inferred).
- **Consequences.** Treating a 404 as success is a risk when the 404 is about something else, for example a missing destination folder. That is why only the specific code counts, and why other 404s fail visibly. GRAPH_CONTRACT 4 note 5 additionally suggests a `GET` of the message to check `parentFolderId` before concluding success; the current core design does not do that (a difference to track). The exact code for a missing destination folder is UNVERIFIED (GRAPH_CONTRACT 11.6).
- **How to revisit.** When the real tenant shows the codes Exchange returns for missing message, missing folder and deleted message.

## D22 - Partial delta updates are resolved in the Graph adapter by a follow-up GET

- **Decision.** Delta may return an updated message as its `id` plus only the changed properties (GRAPH_CONTRACT 2, VERIFIED: "at least the updated properties"). `core::DeltaPage::changed` carries full `MessageSummary` objects, so the Graph adapter completes a partial entry with a follow-up `GET /me/messages/{id}?$select=...` before handing it to the core.
- **Rationale.** Keeps the core simple (it never merges partial rows) and keeps Graph-specific behaviour inside `src/graph`. GRAPH_CONTRACT's own recommendation is to merge partial fields into local rows; this decision resolves the same problem at a different layer.
- **Consequences.** Extra requests per partially updated message (counted against the 4-concurrent and 10,000-per-10-min limits, GRAPH_CONTRACT 8.2) and a window where the message can be gone by the time of the GET (then the entry is treated as a removal). The fake server must emit partial updates in a strict mode to test this (GRAPH_CONTRACT 11.4).
- **How to revisit.** If a large delta makes the follow-up GETs too slow, switch to merging in the core.

## D23 - Cooldown clock strategy

- **Decision.** Within one boot, the cooldown uses the monotonic clock, which a wall-clock change cannot move. Across boots it uses wall time. Anything anomalous (a backward wall jump, a monotonic clock that goes backwards, a missing reading) fails open: the user is allowed to refresh and is never locked out. Failed and partial syncs never update the stored timestamps.
- **Rationale.** Handover section 6 and A16 require that clock changes neither silently bypass the cooldown nor lock the user permanently. They cannot both be fully satisfied on a device without a trusted time source, and a permanent lock-out of a personal tool is the worse failure.
- **Consequences.** Setting the wall clock forward and rebooting, or reinstalling, bypasses the cooldown (accepted, RISKS R-COOLDOWN-CLOCK). Setting the clock backward across a reboot makes the user eligible (reason `clock_anomaly`). The device clock may also be wrong after a long sleep (ASM-29). There is no server-time check, because that would need a network call that is itself the thing being limited.
- **How to revisit.** If a trusted time source becomes available on the device, or if the cooldown becomes a paid-tier control.

## D24 - Device launch route: takeover first, AppLoad native QML later

- **Decision.** The MVP targets route A, "takeover", for the Stage 1 device gate: over SSH, stop `xochitl`, run the standalone Qt Quick binary with `QT_QUICK_BACKEND=epaper ./slowmail -platform epaper`, and restart `xochitl` when it exits (`packaging/run-slowmail.sh`, which always restarts `xochitl` from a trap). Route B, an AppLoad native QML app (QML frontend loaded inside `xochitl` plus a separate backend process), is the likely long-term path. A draft manifest for it lives in `packaging/appload/` and is marked DRAFT / UNTESTED.
- **Rationale.** Route A reuses the standalone Qt app unchanged and needs no third-party launcher, so it is the smallest thing to test on hardware. It is the route the official SDK guide describes, but that is SNIPPET-level evidence only (COMPATIBILITY section 1, run-time flags; the guide itself was unreachable). External Qt Quick apps through AppLoad's `qtfb` route reportedly do not work on the Paper Pro (COMPATIBILITY section 2, option B; VERIFIED [3P]), which is why route B means a native QML module rather than the same binary.
- **Consequences.** Route A risks: `systemctl stop xochitl` can trigger `remarkable-fail.service`, which reboots the tablet (observed on rM2 3.28.0.172, Paper Pro UNVERIFIED); every `xochitl` restart re-prompts for the passcode and takes about a minute; a SIGKILL of the wrapper leaves `xochitl` stopped until reboot (COMPATIBILITY known breakages 9, 10). Route B would require splitting the UI (QML running inside `xochitl`, built against SDK Qt 6.8.2 and loaded by a 6.10.3 `xochitl`) from a backend process over a local socket, and it would need AppLoad >= 0.6.0 on OS 3.28 (known breakage 1). `core::TriageSession` is deliberately the backend boundary for that split, which is one more reason for D17. The AppLoad QML module is GPL-3.0, which matters for route B (D16).
- **How to revisit.** At the Stage 1 gate (tests/device/CHECKLIST.md). If takeover is unreliable on the Paper Pro, or the passcode re-prompt makes it unusable at 2am, move to route B.
