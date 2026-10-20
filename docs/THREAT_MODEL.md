# Slow Mail - Threat Model

Status: pre-alpha, **not approved**. Last updated: 2026-10-20.

> Do not use Slow Mail with a real work mailbox until section 7 (hard gates) is satisfied and the security owner has signed this document. Nothing here has been tested on a Paper Pro or against a real Microsoft 365 tenant.

Scope: single-mailbox Microsoft 365 inbox triage, delegated sign-in (device-code flow), local-first native app, no backend, no cloud relay. Controls are described as they are designed in the headers under `src/`; "implemented" means present in the source tree, not "proven on a device". Tags VERIFIED / UNVERIFIED are as in [ASSUMPTIONS.md](ASSUMPTIONS.md).

## 1. Assets

| Asset | Where it lives | Sensitivity | Notes |
|---|---|---|---|
| OAuth refresh and access tokens | Opaque JSON blob in the local SQLite database (`Store::put_token_blob`), plaintext, file mode 0600 | Critical: a refresh token gives delegated `Mail.ReadWrite` on the user's mailbox until revoked | No keystore on the device as far as is known (ASM-28, D13). The refresh token for native apps has no fixed lifetime (GC 1.6). |
| Mail content (bodies, previews, subjects) | `message_cache` / body cache in the same database | High: corporate confidentiality, personal data | Plain text only; original HTML is not stored. |
| Sender and recipient addresses, display names | Message cache, session-event snapshots, `AccountInfo.display_address` | Medium-High: personal data | Treated as sensitive and never logged. |
| Session journal and command queue | Same database | Medium: reveals what the user chose to delete or defer | Includes subject/sender snapshots (D12). |
| Cache database file and its `-wal` / `-shm` files | `$SLOWMAIL_DATA_DIR` or `/home/root/.local/share/slowmail` on device | Same as its contents | Created 0600; directory 0700. |
| Client id and tenant id | Environment variables | Low: identifiers, not secrets | Never committed. |
| Integrity of mailbox state | Exchange Online | High: a wrong move or delete is user-visible harm | Delete is a soft move (D19); no permanent delete. |

## 2. Actors

| Actor | Capability assumed |
|---|---|
| Owner / legitimate user | Holds the device, signs in with a second device. |
| Thief or finder of a lost device | Physical access; may or may not know the lock passcode. |
| Local attacker with root on the device | Developer mode gives SSH root; secure boot is weakened (COMPAT 4). Can read everything in `/home` while the volume is unlocked. |
| Network attacker (open Wi-Fi) | Can observe or tamper with traffic; cannot break TLS with verified certificates. |
| Malicious sender | Crafts a message (HTML, attachments metadata, Unicode abuse) that the user will triage. |
| Malicious or compromised launcher or third-party app on the same device | Runs as root; can read the app's data. Launchers (xovi, AppLoad) hook into `xochitl` (COMPAT 2). |
| Tenant admin / Microsoft | Can revoke sessions, block device code, block the device through Conditional Access. Trusted, but their policy can change under the app. |
| Supply-chain attacker | Tampers with a dependency, the SDK installer or a launcher release. |
| Compromised build environment | This cloud sandbox produces the sources and tests; see trust boundary 5. |

## 3. Trust boundaries

1. **Device OS in developer mode.** Everything inside the device (xochitl, launcher, Slow Mail, its data) trusts a root user and a weakened boot chain. Disk encryption remains (COMPAT 4). Slow Mail does not defend against root.
2. **Wi-Fi.** Untrusted. Only TLS with certificate verification crosses it. Verification cannot be turned off in `CurlTransport`.
3. **Microsoft Graph and Entra.** Trusted for correctness of responses, but responses are still parsed defensively, sanitised and size-bounded (GC 6, 8).
4. **Launcher layer (xovi, AppLoad, remagic).** Third-party GPL/MIT code running as root, hooking the stock UI. Not audited. Known to crash-loop on some OS versions (R-LAUNCHER-3.28).
5. **This cloud build environment.** The code and docs were produced in a sandbox with restricted network access and no hardware. Outputs are not trusted until built and read by a human on a trusted host. No secrets, client ids or mailbox data are present here.
6. **The second device used for sign-in.** The user types a code on a phone or computer. A phishing attack can ask a victim to enter an attacker's device code; this is the reason Microsoft calls the flow high-risk (GC 1.8).

## 4. STRIDE analysis

| Threat | Example | Control in this design | Gap |
|---|---|---|---|
| **S**poofing | Fake Graph or Entra endpoint; attacker-supplied device code; spoofed sender in a message | TLS verification always on, no redirects followed, authority and base URL fixed unless overridden by env (overrides exist for the fake server only; an attacker who controls the environment is already inside boundary 1). Sender is shown as data, not trusted. | No certificate pinning. Env overrides could point at an attacker server if the environment is tampered. A user could be socially engineered into entering a device code. |
| **T**ampering | Modified traffic; tampered cache DB; tampered binary or launcher | TLS. Server state is the source of truth; cache reconciles after refresh (invariant 8). Never claims an action succeeded before Graph confirms. | No integrity protection of the DB or the binary against a local root attacker. No signed updates (README: manual install only). |
| **R**epudiation | "I never deleted that message" | Local session journal with Pending / Confirmed / Failed (D12). Delete is recoverable (D19). | Journal is local and mutable; not an audit log. |
| **I**nformation disclosure | Tokens or content in logs; DB read from a lost device; HTML tracking pixel phones home; attachment bytes pulled; crash dumps | `redact()` scrubs bearer tokens, JWTs and email addresses; callers log ids and counts only; DB 0600; `secure_wipe`; no attachment bytes (metadata `$select`); sanitiser removes remote references; no web view; `Authorization` and `device_code` never logged. | **No encryption at rest.** Plaintext tokens on a root-accessible device. Core dumps are not explicitly disabled (UNVERIFIED). Filesystem remnants after `unlink` (flash wear levelling) cannot be guaranteed erased. |
| **D**enial of service | Malicious huge message or attachment list; Graph throttling; delta token loop; SQLite corruption | Length, count and body caps (`Limits`); response-size cap; `Retry-After` honoured; bounded page loop; WAL + `synchronous=FULL`. | Cap values (8 MiB response, 200,000 body characters, 100 attachments listed) are chosen by us, not measured on device. Launcher crash-loop is outside our control. |
| **E**levation of privilege | Malicious content executes code; over-broad Graph scope; app runs as root on device | Plain-text rendering only; scopes limited to `offline_access User.Read Mail.ReadWrite` (no application permission, no `Mail.Send` yet). | The app runs as root on the device (developer-mode default). A parser bug would be a root bug. |

## 5. Controls implemented

Present in the source tree (claims tested per TEST_PLAN; none proven on hardware):

- TLS verification always on in `CurlTransport` (peer and host verification, TLS 1.2 minimum); there is no option to disable it, only to set a CA bundle path. Plain `http` is accepted only for loopback URLs (the fake server); every other URL must be `https`.
- No HTTP redirects followed (`CURLOPT_FOLLOWLOCATION` off), so a redirect cannot carry the bearer token to another host.
- Response size cap of 8 MiB in the transport (`kMaxResponseBytes`), reported as a Network error.
- No content in logs: `core::redact()` as a safety net, and a logging rule of ids and counts only. Error messages stored in commands are redacted.
- Database file created mode 0600, data directory 0700; WAL and `synchronous=FULL`.
- `Store::secure_wipe()`: deletes all rows, `VACUUM`, closes, and unlinks db, `-wal` and `-shm` (guarantees limited by the filesystem and flash).
- Sign-out clears the token blob (`DeviceCodeAuth::sign_out`); this does **not** revoke anything server-side.
- No attachment bytes: metadata via `$select`, no `contentBytes`, no `$value`, no `$expand=item`.
- HTML sanitiser (`html_to_text`) and control-character stripping for every single-line field, UTF-8 safe truncation, attachment metadata bounds.
- Least-privilege delegated scopes; client id and tenant id from the environment, never committed.
- Soft delete only; durable command journal with idempotency keys; `InFlight` persisted before network calls.
- No background sync, no push, no analytics, no crash reporting, no cloud relay.
- `scripts/install-device.sh` refuses to run without `SLOWMAIL_I_HAVE_A_BACKUP=1`.

## 6. Controls NOT implemented

| Missing control | Consequence | Reference |
|---|---|---|
| Encryption at rest (tokens, cache) | Root or physical access with the volume unlocked exposes tokens and mail. Do not describe the DB as encrypted. | D13, R-TOKEN-AT-REST |
| Token binding / proof-of-possession | A stolen refresh token works from any machine until revoked. | - |
| Remote wipe | A lost device cannot be told to wipe itself. The only remote action is revoking the tokens at Microsoft (section 8). | - |
| Keystore / secret storage | None known on the device (ASM-28). | COMPAT 5, 8.10 |
| Certificate pinning | Relies on the platform CA bundle. | - |
| Code signing / secure update | Manual copy install only. | R-SUPPLY-CHAIN |
| Disabling core dumps and swap exposure | Not verified. | - |
| Device-compliance enforcement | Up to Conditional Access / Intune, not this app. | R-DEVMODE-SECBOOT |

## 7. Hard gates before a real corporate mailbox is used

All must be true and recorded in TEST_LOG.md:

1. Device owner approval of developer mode, factory reset and unsupported installation (D14), with a verified backup.
2. Tenant-admin approval of the app registration, delegated `Mail.ReadWrite`, and device-code sign-in; Conditional Access / Intune device-compliance decision made (R-DEVMODE-SECBOOT, R-CA-DEVICECODE).
3. Explicit security-owner decision that plaintext tokens on a modified tablet are acceptable, or encryption implemented and tested (R-TOKEN-AT-REST).
4. Everything first exercised on a synthetic tenant and mailbox, including sign-in, revoke and wipe (section 8).
5. This threat model reviewed and signed by the security owner.
6. Stage 1 device gate passed and a launcher / OS version pair recorded as working (tests/device/CHECKLIST.md).
7. Contract tests for move, flag, delta, immutable ids and soft delete run against the synthetic tenant, not only the fake server (ASM-33).
8. TLS to the Graph and Entra hosts verified from the device itself (ASM-18).

## 8. Runbook: revoke, sign out, lost device

Steps marked (UNVERIFIED) are from memory or generic Microsoft behaviour and must be checked on the synthetic tenant before relying on them.

**A. Normal sign-out on the device.** Use the app's sign-out. It clears the token blob. Choose "wipe cache" to run `secure_wipe`; any queued, unsent commands are lost (R-WIPE-DATA-LOSS). Sign-out does not tell Microsoft to revoke the refresh token.

**B. Revoke access at Microsoft (do this for any suspected exposure, and after every device loss).**
1. The user, or the tenant admin, revokes the user's sign-in sessions. The Graph action is `POST /users/{id}/revokeSignInSessions` and the user-facing equivalent is in the account security page (UNVERIFIED; exact paths and labels not checked).
2. The tenant admin can remove the user's consent grant for the Slow Mail app, or disable the app registration (Entra admin centre; UNVERIFIED wording).
3. Reset the user's password if credentials may also be exposed (does not by itself revoke all tokens; UNVERIFIED).
4. Review the sign-in logs for the app's client id for activity after the loss.
Expect access tokens already issued to remain valid until they expire (about an hour, GC 1.5 `expires_in` 3599); revoking stops refresh.

**C. Lost or stolen device.**
1. Do step B immediately.
2. If the device was locked with a passcode and `/home` is encrypted, the risk is lower but not zero in developer mode (boundary 1).
3. Contact the tenant admin; if the device was enrolled or compliance-checked, mark it lost.
4. Tell the device owner to use the manufacturer's account tools to deregister the device (UNVERIFIED; not researched).

**D. Wipe local data deliberately.** Sign-out with wipe (A), or on the device remove the data directory `/home/root/.local/share/slowmail` over SSH. To remove the app, run `scripts/rollback-device.sh` (UNTESTED).

**E. Rotate credentials.** Remove the user's consent grant and sign in again with a new device code. Rotate the client id only if the app registration itself is considered compromised (it is a public client with no secret).

**F. Recover the device.** If the launcher crash-loops, reboot to the stock UI and follow the recovery notes in COMPATIBILITY section 4 (official recovery tool). Leaving developer mode requires the recovery tool.
