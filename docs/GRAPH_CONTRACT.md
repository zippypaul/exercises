# Slow Mail - Microsoft Graph / Entra Contract

Retrieved: 2026-10-20. Scope: single-mailbox Microsoft 365 inbox triage, delegated user sign-in, public client, no backend, Microsoft Graph **v1.0** from C++ via libcurl.

## 0. How to read this document

### Status legend (applied to every claim)

| Tag | Meaning |
|---|---|
| **VERIFIED** | I read the primary text (Microsoft docs markdown, fetched from GitHub raw) in this session. The file and its `ms.date` are cited. |
| **SNIPPET** | Seen only in a web-search result snippet, or on a secondary (non-Microsoft) page. Treat as a lead, not a contract. |
| **UNVERIFIED** | From model memory or inference. No source read. Must be confirmed against a real tenant before the fake server hard-codes it. |

Examples labelled **COPIED** are verbatim from a source file. Examples labelled **ILLUSTRATIVE** were composed by me from verified field names/shapes and are not Microsoft samples.

### How sources were read

`learn.microsoft.com`, `devblogs.microsoft.com` and `web.archive.org` were DNS-blocked. I read the raw Markdown of the docs repos instead. These are the same files that publish to learn.microsoft.com, but `main` can be ahead of or behind the published page.

- Graph docs: `https://raw.githubusercontent.com/microsoftgraph/microsoft-graph-docs-contrib/main/<path>` (abbreviated **GDC:** below)
- Entra docs: `https://raw.githubusercontent.com/MicrosoftDocs/entra-docs/main/docs/<path>` (abbreviated **EDC:** below)
- Support article: `https://raw.githubusercontent.com/MicrosoftDocs/SupportArticles-docs/main/support/entra/entra-id/app-integration/...`
- Published equivalents (not read): `https://learn.microsoft.com/graph/...` and `https://learn.microsoft.com/entra/...`

### Headline findings vs. the four working assumptions

| # | Assumption | Verdict | Why |
|---|---|---|---|
| a | archive = well-known folder `archive` | **Mostly holds, with caveats** | `archive` is a documented well-known name (VERIFIED), but it is the **One-Click Archive folder**, explicitly "not the same as the Archive Mailbox feature of Exchange online". Graph does not (reliably) reach the Online Archive mailbox (SNIPPET). Whether `GET /me/mailFolders/archive` always succeeds for a mailbox that never used Archive is **UNVERIFIED**; handle 404. |
| b | move with ImmutableId keeps id stable | **Holds for the id; contradicted for delta reliability** | Id is stable across folder moves within a mailbox (VERIFIED, `outlook-immutable-id.md`, 2024-11-07). But the **Known Issues** page (VERIFIED, include dated 2026-02-25) says `/delta` calls on messages using immutable IDs can **miss change notifications** when a message moves out of a folder and is later moved back in. The move page's wording ("creates a new copy ... removes the original") conflicts with id stability. Also: the header applies per request, so the move request itself must carry it. |
| c | delta reports moved-out messages as `@removed` | **Holds, but indistinguishable from deletion, and unreliable with immutable IDs** | VERIFIED in `message-delta.md`: `@removed` with `"reason": "deleted"` is emitted "when an item is deleted **or moved from the folder**". The reason does not tell you which. The immutable-ID move-out-and-back hole (above) means a re-appearance may be missed entirely. |
| d | device-code flow works for public clients with offline_access | **Technically holds; operationally risky** | VERIFIED: refresh token is issued when `offline_access` is requested; "Allow public client flows" must be on. But Microsoft's Conditional Access docs (EDC, `ms.date 2026-03-24`) call device code "a high-risk authentication method" and "recommend blocking device code flow wherever possible", and sessions get "protocol tracked", so even refreshes can be blocked later (`AADSTS530036`). Many managed tenants will refuse it. `slow_down` is **not** in Microsoft's documented error table (VERIFIED absence). |

Other surprises worth knowing:

- `POST .../move` returns **201 Created** (not 200) (VERIFIED).
- Delta with `$filter` returns "only up to 5,000 messages" (VERIFIED).
- Updated messages in delta carry "at least the updated properties", not the whole message (VERIFIED) - merge, do not replace.
- Default attachment listing returns `contentBytes` for file attachments (VERIFIED from the sample response) - always `$select`.
- Mailbox concurrency limit is **4** requests per app+mailbox (VERIFIED); 10,000 requests/10 min (VERIFIED).
- Microsoft's own doc samples contain typos (e.g. `"isRead": "true"` as a string, and `@odata.type` without `#`). The fake server must emit the real, typed form (see section 11).

---

## 1. OAuth 2.0 device authorization grant (Microsoft identity platform)

**Status: VERIFIED** for endpoints, parameters, response fields, documented error table, success response, refresh grant, public-client setting and Conditional Access. **UNVERIFIED** for `slow_down` on Entra, numeric AADSTS codes, exact JSON of polling errors, and incremental-consent behaviour.

**Sources (retrieved 2026-10-20):**
- EDC `identity-platform/v2-oauth2-device-code.md` (ms.date 2025-01-04) - published: https://learn.microsoft.com/entra/identity-platform/v2-oauth2-device-code
- EDC `identity-platform/v2-oauth2-auth-code-flow.md` (section "Refresh the access token") - https://learn.microsoft.com/entra/identity-platform/v2-oauth2-auth-code-flow
- EDC `identity-platform/msal-client-applications.md` - https://learn.microsoft.com/entra/identity-platform/msal-client-applications
- SupportArticles `confidential-client-application-authentication-error-aadsts7000218.md` (ms.date 2025-04-30)
- EDC `identity/conditional-access/concept-authentication-flows.md` and `policy-block-authentication-flows.md` (both ms.date 2026-03-24)
- EDC `identity-platform/scenario-desktop-acquire-token-device-code-flow.md` (ms.date 2022-10-07)

### 1.1 Step 1: device authorization request (COPIED)

```http
POST https://login.microsoftonline.com/{tenant}/oauth2/v2.0/devicecode
Content-Type: application/x-www-form-urlencoded

client_id=00001111-aaaa-2222-bbbb-3333cccc4444
&scope=user.read%20openid%20profile
```

| Parameter | Required | Notes (VERIFIED) |
|---|---|---|
| `tenant` (path) | yes | `common`, `consumers`, `organizations`, or a tenant GUID / friendly name |
| `client_id` | yes | Application (client) ID from the app registration |
| `scope` | yes | Space-separated scopes |

"From the moment the request is sent, the user has 15 minutes to sign in. This is the default value for `expires_in`." Request a device code only when the user says they are ready to sign in.

For Slow Mail:

```
scope=offline_access User.Read Mail.ReadWrite
```
(URL-encode spaces as `%20` or `+`.) Add `openid profile` only if you want an `id_token` (VERIFIED: "`id_token` ... Issued if the original `scope` parameter included the `openid` scope"). `offline_access` is what yields a `refresh_token`.

### 1.2 Device authorization response

Fields (VERIFIED, table in v2-oauth2-device-code.md):

| Field | Type | Meaning |
|---|---|---|
| `device_code` | string | Long opaque string used when polling `/token` |
| `user_code` | string | Short code the user types on the second device |
| `verification_uri` | URI | Where the user goes |
| `expires_in` | int | Seconds until `device_code`/`user_code` expire |
| `interval` | int | Seconds the client must wait between polls |
| `message` | string | Human-readable instruction; localisable with `?mkt=xx-XX` on the request |

`verification_uri_complete` "is not included or supported at this time" (VERIFIED). Do not rely on it.

ILLUSTRATIVE example (values are placeholders, shape from the field table):

```json
{
  "user_code": "ABCD1234",
  "device_code": "GMMhmHCXhWEzkobqIHGG_EnNYYsAkukHspeYUk9E8...",
  "verification_uri": "https://microsoft.com/devicelogin",
  "expires_in": 900,
  "interval": 5,
  "message": "To sign in, use a web browser to open the page https://microsoft.com/devicelogin and enter the code ABCD1234 to authenticate."
}
```
`expires_in: 900` matches "15 minutes" (VERIFIED). The exact `verification_uri` value and default `interval` are **UNVERIFIED** (the scenario doc only mentions `https://microsoft.com/devicelogin` as an example of a URL). Always display `message` or `verification_uri`+`user_code` exactly as returned.

### 1.3 Step 2: poll the token endpoint (COPIED)

```http
POST https://login.microsoftonline.com/{tenant}/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=00001111-aaaa-2222-bbbb-3333cccc4444&device_code=GMMhmHCXhWEzkobqIHGG_EnNYYsAkukHspeYUk9E8...
```
`tenant` must match the devicecode call, `client_id` must match, `grant_type` must be exactly the URN above. No `client_secret` (public client).

### 1.4 Documented polling errors (VERIFIED, table "Expected errors")

| `error` | Meaning | Client action (per Microsoft) |
|---|---|---|
| `authorization_pending` | User has not finished, has not cancelled | Repeat after at least `interval` seconds |
| `authorization_declined` | User denied | Stop polling; unauthenticated state |
| `bad_verification_code` | `device_code` not recognised | Check the `device_code` being sent |
| `expired_token` | `expires_in` exceeded | Stop polling; unauthenticated state |

**`slow_down`: UNVERIFIED for Entra.** It is defined by RFC 8628 (device authorization grant) and is **absent from Microsoft's table** (VERIFIED absence; a search found no Microsoft page documenting it - SNIPPET). Implement it defensively per RFC 8628 (add 5 s to the interval, cumulatively) because it costs nothing, and make the fake server able to emit it, but do not assert that real Entra emits it.

Polling error body shape: the generic Entra token-error JSON is VERIFIED from the refresh-token error sample (fields `error`, `error_description`, `error_codes`, `timestamp`, `trace_id`, `correlation_id`). HTTP status for pending is 400 (SNIPPET from generic RFC 8628 pages; the Microsoft page does not state it). ILLUSTRATIVE:

```http
HTTP/1.1 400 Bad Request
Content-Type: application/json

{
  "error": "authorization_pending",
  "error_description": "AADSTS70016: OAuth 2.0 device flow error. Authorization is pending. Continue polling. Trace ID: ... Correlation ID: ... Timestamp: ...",
  "error_codes": [70016],
  "timestamp": "2026-10-20 10:00:00Z",
  "trace_id": "0000aaaa-11bb-cccc-dd22-eeeeee333333",
  "correlation_id": "aaaa0000-bb11-2222-33cc-444444dddddd"
}
```
`70016` for pending and `70019` for expired are **UNVERIFIED** (memory). Branch on the `error` string, never on `error_codes` or message text.

### 1.5 Success response (COPIED)

```json
{
    "token_type": "Bearer",
    "scope": "User.Read profile openid email",
    "expires_in": 3599,
    "access_token": "eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIsIng1dCI6Ik5HVEZ2ZEstZnl0aEV1Q...",
    "refresh_token": "AwABAAAAvPM1KaPlrEqdFSBzjqfTGAMxZGUTdM0t4B4...",
    "id_token": "eyJ0eXAiOiJKV1QiLCJhbGciOiJub25lIn0.eyJhdWQiOiIyZDRkMTFhMi1mODE0LTQ2YTctOD..."
}
```
Verified field notes: `token_type` always `Bearer`; `scope` = scopes the access token is valid for; `refresh_token` only if `offline_access` requested; `id_token` only if `openid` requested. Microsoft also tells apps not to validate access tokens (the token is for Graph, not for you).

Real `scope` values for Graph come back as e.g. `Mail.ReadWrite User.Read profile openid email`; compare scope names **case-insensitively** (the sample above shows `User.Read`, the request used `user.read`).

### 1.6 Refresh-token grant (COPIED from the auth-code doc; applies to tokens from any flow)

```http
POST /{tenant}/oauth2/v2.0/token HTTP/1.1
Host: https://login.microsoftonline.com
Content-Type: application/x-www-form-urlencoded

client_id=00001111-aaaa-2222-bbbb-3333cccc4444
&scope=https%3A%2F%2Fgraph.microsoft.com%2Fmail.read
&refresh_token=OAAABAAAAiL9Kn2Z27UubvWFPbm0gLWQJVzCTE9UkP3pSx1aXxUjq...
&grant_type=refresh_token
```
(The sample also shows `client_secret`; Microsoft's table says it is "required for web apps" and "shouldn't be used in a native app". **Omit it.**)

Verified semantics:
- `scope` is optional; if given it must be "equivalent to or a subset of the scopes requested in the original" request.
- "Refresh tokens are valid for all permissions that your client has already received consent for."
- "Refresh tokens aren't revoked when used to acquire new access tokens. You're expected to discard the old refresh token" and "replace the old refresh token with this newly acquired refresh token". The response contains a new `refresh_token` only if `offline_access` applies.
- Refresh tokens for native apps "don't have specified lifetimes", but "in some cases, refresh tokens expire, are revoked, or lack sufficient privileges". Handle `invalid_grant` / `interaction_required` by restarting sign-in.
- The 24-hour lifetime applies only to `spa` redirect URIs (not us).

### 1.7 App-registration requirements

- **Allow public client flows = Yes** (VERIFIED, SupportArticles AADSTS7000218): Azure portal > App registrations > your app > Authentication > Advanced settings > "Allow public client flows" > "Enable the following mobile and desktop flows" = **Yes**. Without it the token call fails with `invalid_client` / `AADSTS7000218: The request body must contain the following parameter: 'client_assertion' or 'client_secret'` (VERIFIED error text, COPIED from that page).
- `msal-client-applications.md` (VERIFIED): the public client flow setting "by default ... should be disabled unless" building a public client using, among other things, device code flow; changing client type has "security implications".
- Supported account types decides which `{tenant}` you can use. For a single M365 organization use the tenant GUID/domain or `organizations`.
- API permissions to add (delegated, Microsoft Graph): `offline_access`, `User.Read`, `Mail.ReadWrite`; later `Mail.Send`. Whether user consent suffices or the tenant requires admin consent is tenant policy: **UNVERIFIED** for each (do not assume).
- Incremental consent for `Mail.Send`: refresh-token `scope` must be a subset of already-consented scopes (VERIFIED), so adding `Mail.Send` later requires a **new interactive sign-in** (a new device code request with the larger scope). Exact error returned when you try via refresh is **UNVERIFIED**.

### 1.8 Conditional Access and device-code blocking (VERIFIED, EDC ms.date 2026-03-24)

- "Device code flow is a high-risk authentication method that can be part of a phishing attack or used to access corporate resources on unmanaged devices." "Microsoft recommends blocking device code flow wherever possible."
- Admin configures it under Conditional Access > Conditions > **Authentication Flows** > Device code flow, with grant control **Block access** (policy page recommends starting in Report-only).
- **Protocol tracking:** a session started with device code is "protocol tracked"; tracking "is sustained through subsequent refreshes", so later token refreshes (even via other flows) can be blocked by an authentication-flows policy. Symptom code: `AADSTS530036: The refresh token is invalid due to authentication flow checks by Conditional Access.` Per Microsoft there is "no recommended remediation when the policy is in `enabled` state" and the user may be fully signed out.
- Since early September 2024, authentication-flows policies targeting **all resources** also apply to the Device Registration Service (VERIFIED), irrelevant to us.
- 403 with `error=insufficient_claims` can also be returned by Graph when CA applies to the resource (VERIFIED in `errors.md`).

### Implementation notes for Slow Mail

1. Treat device code as **one sign-in strategy, not the only one**. Design the auth layer behind an interface with two implementations: (i) auth code + PKCE with a loopback redirect (`http://localhost`, registered as "Mobile and desktop applications") - this is **UNVERIFIED** here (not read in this session; confirm against `scenario-desktop-app-registration`/MSAL docs) - and (ii) device code. Offer device code when no browser is available on the machine, but expect `AADSTS` blocks in hardened tenants and show a clear "your admin blocks device code sign-in" message when the token endpoint reports a Conditional Access failure.
2. Polling loop: sleep `interval` seconds (never less); on `authorization_pending` continue; on `slow_down` add 5 s (defensive); on `expired_token`, `authorization_declined`, `bad_verification_code` stop and surface a distinct UI state. Hard-stop at `expires_in` regardless.
3. Request exactly `offline_access User.Read Mail.ReadWrite` for phase 1. Do **not** request `Mail.Send` until the send feature ships.
4. Persist the refresh token in the OS credential store (not a plain file). On every refresh, atomically store the **new** refresh token before discarding the old one.
5. Refresh the access token proactively when less than ~5 minutes remain, and reactively once on a 401 (section 8). `invalid_grant`/`interaction_required`/`AADSTS530036` on refresh means: clear tokens, go to sign-in.
6. Never send `client_secret`. Never log tokens or `device_code`.
7. Tenant selection: configurable, default `organizations`; pin to the tenant after the first sign-in (from `id_token` `tid` if `openid` requested - UNVERIFIED claim name, standard but not read here).

---

## 2. Inbox delta: `GET /me/mailFolders/{id}/messages/delta`

**Status: VERIFIED** (query options, headers, shapes, `@removed`, token tracking, token duration, 5,000 filter cap, `changeType`). **UNVERIFIED**: using the literal `inbox` well-known name in the delta URL, max page size, exact `@removed` behaviour per scenario, the exact error code on expired token.

**Sources (retrieved 2026-10-20):**
- GDC `api-reference/v1.0/api/message-delta.md` (ms.date 2024-06-21; contains the newer @removed note) - https://learn.microsoft.com/graph/api/message-delta
- GDC `concepts/delta-query-messages.md` (ms.date 2024-11-07) - https://learn.microsoft.com/graph/delta-query-messages
- GDC `concepts/delta-query-overview.md` - https://learn.microsoft.com/graph/delta-query-overview
- GDC `concepts/includes/known-issues/mail.md` (ms.date 2026-02-25) - https://learn.microsoft.com/graph/known-issues
- Permissions: delegated least privileged `Mail.ReadBasic`; higher: `Mail.Read`, `Mail.ReadWrite` (VERIFIED, `message-delta-permissions.md`). `Mail.ReadWrite` suffices.

### 2.1 Request shape

```http
GET https://graph.microsoft.com/v1.0/me/mailFolders/{inboxId}/messages/delta?$select=subject,from,receivedDateTime,isRead,flag,bodyPreview,parentFolderId,hasAttachments,conversationId,internetMessageId,importance,categories&$filter=receivedDateTime ge 2026-09-20T00:00:00Z&$orderby=receivedDateTime desc
Authorization: Bearer {token}
Prefer: odata.maxpagesize=50, IdType="ImmutableId"
```
(The two Prefer values are shown comma-joined; whether Graph accepts comma-joined vs separate `Prefer:` header lines is **UNVERIFIED** - RFC 7240 allows both. Verify against the real service; make the fake server accept both.)

Verified rules:
- Supported on messages: `$select`, `$top`, `$expand`. **`id` is always returned.**
- `$filter`: the **only** supported expressions are `receivedDateTime ge {value}` or `receivedDateTime gt {value}`. "Applying `$filter` in a delta query returns only up to 5,000 messages."
- `$orderby`: the only supported expression is `receivedDateTime desc`. Without it "the return order isn't guaranteed".
- **No `$search`.**
- "If you use any query parameter (other than `$deltatoken` and `$skiptoken`) that is an OData system query option or the custom query option `changeType`, you must specify it in the initial delta request." Graph encodes them in the returned token; later requests must just follow the returned URL verbatim.
- `Prefer: odata.maxpagesize={x}` optional. Documented max value for messages: **UNVERIFIED** (a forum answer about *contacts* delta says the server caps requested sizes at 200 - SNIPPET, not applicable by assumption).
- `changeType` **is** supported for messages: `created`, `updated`, `deleted` (VERIFIED, both `message-delta.md` and `delta-query-messages.md`). Example 2 of the concept doc shows that with `changeType=created`, updated and deleted messages are not returned.
- Delta is **per folder**: track each folder separately; moving a message between tracked folders appears in both.
- "Delta queries for messages can return change events that don't match the filter conditions specified in the initial request": `@removed` entries with `"reason": "deleted"` when an item is "deleted or moved from the folder", and read/unread state changes. These "are **not** filtered out" (VERIFIED, note in `message-delta.md`).
- Folder in URL: docs always use a folder **id** in the delta path. The literal `inbox` well-known name in the path is **UNVERIFIED** here. Resolve the inbox id once (section 7) and use the id.

### 2.2 Response: nextLink page (COPIED from the concept doc)

```json
{
  "@odata.context": "https://graph.microsoft.com/v1.0/$metadata#Collection(message)",
  "@odata.nextLink": "https://graph.microsoft.com/v1.0/me/mailfolders('AQMkADNkNAAAgEMAAAA')/messages/delta?$skiptoken=GwcBoTmPuoTQWfcsAbkYM",
  "value": [
    {
      "@odata.type": "#microsoft.graph.message",
      "@odata.etag": "W/\"CQAAABYAAAARn2vdzPFjSbaPPxzjlzOTAAASsKZz\"",
      "subject": "Holiday hours update",
      "isRead": false,
      "sender": {
        "emailAddress": {
          "name": "Dana Swope",
          "address": "danas@contoso.com"
        }
      },
      "id": "AAMkADNkNAAASq35xAAA="
    }
  ]
}
```
(The concept doc's prose says the link is a "response header"; its JSON samples show it as a **body property**. Read it from the body.)

### 2.3 Response: final page with deltaLink (COPIED)

```json
{
  "@odata.context": "https://graph.microsoft.com/v1.0/$metadata#Collection(message)",
  "@odata.deltaLink": "https://graph.microsoft.com/v1.0/me/mailfolders('AQMkADNkNAAAgEMAAAA')/messages/delta?$deltatoken=GwcBoTmPuoGNlgXgF1nyUNMXY",
  "value": [ ... ]
}
```
Verified: a page never contains both `nextLink` and `deltaLink`; a `nextLink` page can be **empty**; `deltaLink` means "no more data about the existing state ... in the current session"; save the **entire** URL.

### 2.4 Subsequent round (COPIED)

```http
GET https://graph.microsoft.com/v1.0/me/mailfolders/AQMkADNkNAAAgEMAAAA/messages/delta?$deltatoken=GwcBoTmPuoGNlgXgF1nyUNMXY HTTP/1.1
Prefer: odata.maxpagesize=2
```
Response (COPIED): one deleted and one changed message:

```json
{
  "@odata.context": "https://graph.microsoft.com/v1.0/$metadata#Collection(message)",
  "@odata.deltaLink": "https://graph.microsoft.com/v1.0/me/mailfolders('AQMkADNkNAAAgEMAAAA')/messages/delta?$deltatoken=GwcBoTmPuoGNlgXgF1nyUNMXY",
  "value": [
    {
      "@odata.type": "#microsoft.graph.message",
      "id": "AAMkADk0MGFkODE3LWE4MmYtNDRhOS0Dh_6qB-pB2Sa2pUum19a6YAAKnLuxoAAA=",
      "@removed": {
        "reason": "deleted"
      }
    },
    {
      "@odata.type": "#microsoft.graph.message",
      "@odata.etag": "W/\"CQAAABYAAAARn2vdzPFjSbaPPxzjlzOTAAASsKZz\"",
      "subject": "Holiday hours update",
      "isRead": "true",
      "sender": { "emailAddress": { "name": "Dana Swope", "address": "danas@contoso.com" } },
      "id": "AAMkADNkNAAASq35xAAA="
    }
  ]
}
```
Doc anomalies to NOT copy into the fake server: `"isRead": "true"` is a string typo in the doc (the resource type is Boolean); the "next round" sample reuses the same `deltatoken` value; Example 2 shows a `deltaLink` containing `$skiptoken`. Be tolerant in the parser (accept string/boolean for `isRead`), but make the fake emit booleans and proper tokens.

### 2.5 `@removed` semantics

- Shape (VERIFIED): `{"@odata.type":"#microsoft.graph.message","id":"...","@removed":{"reason":"deleted"}}`. Only `id` (+ `@odata.type`) is present.
- Generic delta doc (VERIFIED): possible reasons are `changed` ("deleted and can be restored from deletedItems") and `deleted` ("can't be restored"); the `changed` example is for directory objects. For **messages**, the message-specific docs only show `deleted`, and state it is used for both deleted and moved-out. Whether Exchange ever emits `reason: "changed"` for messages is **UNVERIFIED**. Parser must accept any `@removed` object and any reason.
- A removal tells you "no longer in this folder", **not** why. To distinguish moved vs deleted vs permanently gone, `GET /me/messages/{id}?$select=parentFolderId,isRead` (with immutable-ID header): 200 + different `parentFolderId` = moved; 404 `ErrorItemNotFound` = gone. This is **UNVERIFIED inference**.
- Replays and duplicates (VERIFIED): "Your application must be prepared for replays"; "A single entity can be contained multiple times in the response". Process idempotently, last entry wins in page order (order not guaranteed without `$orderby`; and even with it, "assume that the same item can show up anywhere in the `@odata.nextLink` sequence").
- Updated items (VERIFIED): "represented by their **id** with *at least* the updated properties, but other properties might be included" - **merge** into local rows.
- Eventual consistency (VERIFIED): "Expect varying delays" between a change and its appearance; retrying the link later may show more.

### 2.6 Moves in delta

- Source folder: `@removed` (VERIFIED as above).
- Destination folder (tracked separately): appears as a regular (created-style) entry. With default IDs it has a **new id**; with ImmutableId the same id (inference from section 3; **UNVERIFIED** for delta specifically).
- **Known issue (VERIFIED, `known-issues/mail.md`, ms.date 2026-02-25):** "When you make `/delta` calls to the messages API using immutable Ids in some cases (for example when a message moves out of a folder and is then moved back in), you might miss some change notifications."
- A Microsoft Q&A report corroborates (SNIPPET): with mutable ids you see old item deleted + new item; with ImmutableId only a deleted entry, nothing on return.

### 2.7 Token lifetime, expiry and resync

- Outlook delta tokens (VERIFIED, `delta-query-overview.md`): "For Outlook entities (**message**, **mailFolder**, ...), the upper limit isn't fixed; it's dependent on the size of the internal delta token cache. While new delta tokens are continuously added in the cache, after the cache capacity is exceeded, the older delta tokens are deleted." So a token can be evicted any time, more likely after long inactivity.
- On expiry (VERIFIED): "the service should respond with a 40X-series error with error codes such as `syncStateNotFound`."
- **Synchronization reset** (VERIFIED): "Delta query can return a response code of `410 Gone` and a **Location** header containing a request URL with an empty `$deltatoken` (same as the initial query)... the application must restart with a full synchronization."
- Exact HTTP status and error `code` Exchange uses for messages (400 vs 404 vs 410; `syncStateNotFound` vs `ErrorSyncStateNotFound`/`ErrorInvalidSyncStateData`) is **UNVERIFIED** (a SNIPPET from a 2020 Graph blog describes 400 `syncStateNotFound` for directory/education objects, not mail). ILLUSTRATIVE only:

```http
HTTP/1.1 410 Gone
Location: https://graph.microsoft.com/v1.0/me/mailFolders/{id}/messages/delta?$deltatoken=
Content-Type: application/json

{"error":{"code":"syncStateNotFound","message":"The sync state is not found or has expired.","innerError":{"request-id":"...","date":"..."}}}
```

### Implementation notes for Slow Mail

1. Store per-folder: `folderId`, last `deltaLink` (opaque string, full URL), and the local message rows. Persist the `deltaLink` **only after** all pages of the round are applied (single transaction).
2. Initial sync: bound it with `$filter=receivedDateTime ge {now-N days}` (the 5,000 cap makes unbounded inbox sync unsafe with `$filter`; without `$filter` the cap does not apply per docs, but a huge inbox is slow). Use `Prefer: odata.maxpagesize=50`. Request `$select` of metadata only; **do not select `body`** in delta (fetch on demand, section 6).
3. Treat `@odata.nextLink` / `@odata.deltaLink` as opaque: follow verbatim, never rebuild or append `$select`. Send the same `Prefer` headers on every call (the header is per-request, section 3).
4. Page loop: continue while `@odata.nextLink`; stop at `@odata.deltaLink`; tolerate empty pages. Cap iterations defensively (e.g. 10,000 pages).
5. Apply entries in order: if `@removed` present -> mark gone from this folder (then classify move vs delete lazily, section 2.5); else upsert by `id` merging only present fields.
6. Resync rule: on **any** 4xx from a delta URL other than 401/403/429 (400, 404, 410, with or without a recognised code), discard the stored `deltaLink`, run the initial query again, and **reconcile**: messages in the local store for that folder that were not seen in the full run are removed locally. If a `Location` header is present on a 410, use it as the restart URL.
7. Defend against the known immutable-ID gap: schedule a periodic cheap reconcile (e.g. `GET .../messages?$select=id&$filter=receivedDateTime ge ...&$top=...` page-through, or a forced full resync every N days / on app start after >24 h idle). The "moved out and back in" case would otherwise leave a stale `@removed`-deleted row. **Own-move optimisation:** after a successful own `move`, update the local row immediately and expect an echo in the next delta; do not treat the echo as a surprise.
8. Poll no more often than needed (a "slow mail" app: minutes, not seconds). Do not use `changeType` for the main loop: it hides updates/removals the triage UI needs. (Optional use only for a cheap "new mail since" probe.)
9. Do not re-use deltaLinks across different `$select`/`$filter` settings: changing the query means a fresh initial sync.

---

## 3. Immutable IDs

**Status: VERIFIED** (header, lifetime rules, delta interplay) from `outlook-immutable-id.md` (ms.date 2024-11-07) and the 2026-02-25 known issue. **UNVERIFIED**: behaviour when the header is omitted on the follow-up request, `Preference-Applied` echo, effect inside batch requests.

**Sources (retrieved 2026-10-20):** GDC `concepts/outlook-immutable-id.md` - https://learn.microsoft.com/graph/outlook-immutable-id ; GDC `includes/outlook-immutable-id.md` (text used in the `message.id` property description); GDC `concepts/includes/known-issues/mail.md`.

### 3.1 Request header (COPIED)

```http
Prefer: IdType="ImmutableId"
```
"This header only applies to the request it is included with. If you want to always use immutable IDs, you must include this header with every API request."

Default behaviour (VERIFIED, `message.id` description): "By default, this value changes when the item is moved from one container (such as a folder or calendar) to another."

### 3.2 Lifetime rules (VERIFIED)

"An item's immutable ID won't change so long as the item stays in the same mailbox. That means that immutable ID will NOT change if the item is moved to a different folder in the mailbox. However, the immutable ID changes if:
- The user moves the item to an archive mailbox.
- The user exports the item (to a PST, as an MSG file, etc.) and re-imports it into their mailbox."

Also: IDs "are case-sensitive. Keep this in mind if you are comparing IDs." Supported item types include message and attachment; "Container types (mailFolder, calendar, etc.) don't support immutable ID, but their regular IDs were already constant."

**The Archive-folder vs archive-mailbox distinction matters here:** moving a message into the **`archive` well-known folder** is a move within the mailbox, so the id is stable. Moving into an **Online Archive mailbox** (a different mailbox) changes the id. Slow Mail's `archive` is the folder (section 4).

### 3.2.1 Delta and notifications (VERIFIED)

"You can request that Microsoft Graph return immutable IDs in delta query responses ... by including the `Prefer: IdType="ImmutableId"` header. The `@odata.nextLink` and `@odata.deltaLink` values returned by delta queries are compatible with both ID formats, so your application doesn't need to re-synchronize to take advantage of immutable ID." (So switching the header on mid-stream changes IDs in subsequent pages without requiring resync; but your local store must be migrated: use `translateExchangeIds`.)

Migration of already-stored ids (VERIFIED, COPIED):

```http
POST https://graph.microsoft.com/v1.0/me/translateExchangeIds

{
  "inputIds" : [ "AQMkAGM2…" ],
  "targetIdType" : "restImmutableEntryId",
  "sourceIdType" : "restId"
}
```
```http
HTTP 200 OK

{ "value": [ { "targetId": "AAkALgAA...", "sourceId": "AQMkAGM2..." } ] }
```
Up to 1,000 ids per call (VERIFIED).

### 3.3 Caveats (summary)

| Caveat | Status |
|---|---|
| ID changes on archive-mailbox move or PST/MSG export+re-import | VERIFIED |
| Header must be on **every** request incl. the move request, or the returned/received id is the mutable form | VERIFIED rule ("only applies to the request it is included with"); consequences **UNVERIFIED** |
| `/delta` + immutable ids can miss events when a message leaves a folder and returns | VERIFIED (known issue, 2026-02-25) |
| Move doc says move "creates a new copy ... and removes the original" | VERIFIED text; conflicts with id stability; real id behaviour must be tested |
| Sent-item copy id: create draft with the header, send, then GET with the draft's id (may not succeed immediately) | VERIFIED |
| Mixed-format ids for the same message across calls: a mutable id and an immutable id for the same item are different strings | UNVERIFIED inference; never compare across formats |

### Implementation notes for Slow Mail

1. Send `Prefer: IdType="ImmutableId"` on **every** Graph call (delta, get, move, patch, attachments, reply/forward). Centralise it in the HTTP layer so it cannot be forgotten. Use it from the first sync so you never need `translateExchangeIds`.
2. Key the local DB on `id` (case-sensitive compare) **and** keep `internetMessageId` (RFC 2822 id, stable across moves) as a secondary dedupe/reconcile key.
3. After a move, always replace the local id with the `id` in the move response (identical with immutable ids, different otherwise). Never assume.
4. Add a startup/idle reconcile to cover the known delta gap (section 2 note 7).
5. If you ever see two local rows with the same `internetMessageId` in one mailbox, treat as an id-format transition or delta-gap artefact and merge.

---

## 4. Move: `POST /me/messages/{id}/move`

**Status: VERIFIED** (endpoint, body, 201 + message body, well-known names, permission). **UNVERIFIED**: error responses, whether `archive` exists for every mailbox, id behaviour in the response (depends on the Prefer header).

**Sources (retrieved 2026-10-20):** GDC `api-reference/v1.0/api/message-move.md` (ms.date 2024-04-04) - https://learn.microsoft.com/graph/api/message-move ; GDC `api-reference/v1.0/resources/mailfolder.md` (ms.date 2024-08-23) - https://learn.microsoft.com/graph/api/resources/mailfolder ; permission `Mail.ReadWrite` only (VERIFIED, `message-move-permissions.md`).

### 4.1 Request (COPIED, with the immutable-id header added by us)

```http
POST https://graph.microsoft.com/v1.0/me/messages/AAMkADhAAATs28OAAA=/move
Content-type: application/json
Prefer: IdType="ImmutableId"

{
  "destinationId": "deleteditems"
}
```
`destinationId`: "The destination folder ID, or a well-known folder name."

### 4.2 Response (COPIED, shortened by Microsoft)

```http
HTTP/1.1 201 Created
Content-type: application/json

{
    "@odata.context":"https://graph.microsoft.com/v1.0/$metadata#message",
    "@odata.type":"#microsoft.graph.message",
    "@odata.etag":"W/\"FwAAABYAAAC4ofQHEIqCSbQPot83AFcbAAAW/0tB\"",
    "id":"AAMkADhAAAW-VPeAAA=",
    "createdDateTime":"2018-08-12T08:43:22Z",
    "lastModifiedDateTime":"2018-08-15T19:47:54Z",
    "changeKey":"FwAAABYAAAC4ofQHEIqCSbQPot83AFcbAAAW/0tB",
    "categories":[ ],
    "receivedDateTime":"2018-08-12T08:43:22Z",
    "sentDateTime":"2018-08-12T08:43:20Z",
    "hasAttachments":false,
    "internetMessageId":"<00535324-5988-4b6a-b9af-d44cf2d0b691@MWHPR2201MB1022.namprd22.prod.outlook.com>",
    "subject":"Undeliverable: Meet for lunch?",
    "bodyPreview":"Delivery has failed to these recipients or groups: ...",
    "importance":"normal",
    "parentFolderId":"AAMkADhAAAAAAEKAAA=",
    "conversationId":"AAQkADhJzfbkARFhe5kKhjihSA=",
    "isRead":false,
    "isDraft":false,
    "webLink":"https://outlook.office365.com/owa/?ItemID=...",
    "inferenceClassification":"focused",
    "body":{ "contentType":"html", "content":"<html></html>" },
    "from": { "emailAddress": { "name":"Microsoft Outlook", "address":"..." } },
    "toRecipients":[ ... ],
    "flag":{ "flagStatus":"notFlagged" }
}
```
Verified: **201 Created** (not 200); body is the moved message; its `id` may differ from the request id (default mode); `parentFolderId` is the destination. The page's lead sentence: "This creates a new copy of the message in the destination folder and removes the original message."

### 4.3 Well-known folder names (VERIFIED table, mailfolder.md)

`archive`, `clutter`, `conflicts`, `conversationhistory`, `deleteditems`, `drafts`, `inbox`, `junkemail`, `localfailures`, `msgfolderroot`, `outbox`, `recoverableitemsdeletions`, `scheduled`, `searchfolders`, `sentitems`, `serverfailures`, `syncissues`. "Well-known names work regardless of the locale of the user's mailbox". Names are lowercase in the docs; case-insensitivity is **UNVERIFIED**, so always send lowercase.

**The archive note (COPIED):** "`archive` | The archive folder messages are sent to when using the One_Click Archive feature in Outlook clients that support it. **Note:** This isn't the same as the Archive Mailbox feature of Exchange online."

Archive mailbox reach via Graph: a Microsoft Q&A moderator statement and a 2026 Office365ITPros piece (both SNIPPET) say Graph does not support the Online Archive mailbox content; EWS did. Treat as: **Slow Mail can only target the Archive *folder***.

### Implementation notes for Slow Mail

1. At startup (and after sign-in) resolve and cache folder ids with `GET /me/mailFolders/inbox`, `/archive`, `/deleteditems` (section 7). Use those **ids** as `destinationId` (stable, locale-proof). Keep the well-known string as fallback.
2. If `GET /me/mailFolders/archive` returns 404/`ErrorItemNotFound`: the mailbox has no usable archive folder (existence guarantee is **UNVERIFIED**). Do not silently fall back to `deleteditems`. Show "Archive folder not available in this mailbox", and let the user choose another existing folder (store its id) - that is a deliberate product decision.
3. Do not treat Online Archive as a target; do not message "archived to your archive mailbox".
4. Handle `201` (accept any 2xx). Parse the response; update the local row `id`, `parentFolderId`, `changeKey` from it. If the response is 404 (already moved/deleted by another client) go to section 8.3.
5. Idempotency: a retried move after a timeout may 404 because the first attempt succeeded. On 404 for a move, `GET /me/messages/{id}?$select=parentFolderId`; if `parentFolderId == destination`, treat as success (needs ImmutableId to be reliable; **UNVERIFIED**).
6. Archive vs delete: "Delete" = move to `deleteditems` (recoverable). Permanent delete (`POST .../permanentDelete`) is out of scope for v1.
7. Moves count against the 4-concurrent and 10,000/10-min mailbox limits; batch large triage sessions through the client-side semaphore (section 8).

---

## 5. Follow-up flag: `PATCH /me/messages/{id}`

**Status: VERIFIED** for the `flag` property, `followupFlag` fields, 200 + updated message, and the "dueDateTime requires startDateTime else 400" rule. **UNVERIFIED**: exact flagged/complete/clear request bodies and which extra fields the service enforces (they are composed by me), whether `$select` on a PATCH is honoured, whether `completedDateTime` is required for `complete`.

**Sources (retrieved 2026-10-20):** GDC `api-reference/v1.0/resources/followupflag.md` (ms.date 2024-04-03) - https://learn.microsoft.com/graph/api/resources/followupflag ; GDC `api-reference/v1.0/api/message-update.md` (ms.date 2024-06-21) - https://learn.microsoft.com/graph/api/message-update ; GDC `resources/datetimetimezone.md`. Permission: `Mail.ReadWrite` (VERIFIED). No `If-Match` header is listed for this call (VERIFIED absence).

Stale-doc warning: `outlook-mail-concept-overview.md` (ms.date 2024-11-07) still says flagging "is currently in preview" in Graph; the v1.0 `message` resource and `message-update` pages list `flag` as updatable, so the overview text is out of date (VERIFIED discrepancy).

### 5.1 `followupFlag` resource (VERIFIED, COPIED)

| Property | Type | Description |
|---|---|---|
| `flagStatus` | followupFlagStatus | "The status for follow-up for an item. Possible values are `notFlagged`, `complete`, and `flagged`." |
| `startDateTime` | dateTimeTimeZone | "The date and time that the follow-up is to begin." |
| `dueDateTime` | dateTimeTimeZone | "... To set the due date, you must also specify the `startDateTime`; otherwise, you get a `400 Bad Request` response." |
| `completedDateTime` | dateTimeTimeZone | "The date and time that the follow-up was finished." |

```json
{
  "completedDateTime": {"@odata.type": "microsoft.graph.dateTimeTimeZone"},
  "dueDateTime": {"@odata.type": "microsoft.graph.dateTimeTimeZone"},
  "flagStatus": "String",
  "startDateTime": {"@odata.type": "microsoft.graph.dateTimeTimeZone"}
}
```
`dateTimeTimeZone` (VERIFIED): `{"dateTime": "2017-08-29T04:00:00.0000000", "timeZone": "Pacific Standard Time"}`; `timeZone` can be a Windows time-zone name (and `UTC`; "UTC" being accepted is **UNVERIFIED**).

The `message` resource (VERIFIED) lists `flag` as `followupFlag`. In message JSON it appears as `"flag": {"flagStatus": "notFlagged"}` (VERIFIED in the move and get samples).

### 5.2 Request/response

Update body table (VERIFIED): `flag` - "The flag value that indicates the status, start date, due date, or completion date for the message." Response: "`200 OK` response code and updated message object". The doc's example PATCH is for `subject`/`body`; flag bodies below are ILLUSTRATIVE.

Set flag (ILLUSTRATIVE; matches the task's assumption):

```http
PATCH https://graph.microsoft.com/v1.0/me/messages/{id}
Content-type: application/json
Prefer: IdType="ImmutableId"

{ "flag": { "flagStatus": "flagged" } }
```
With dates (ILLUSTRATIVE; start required if due present):

```json
{ "flag": {
    "flagStatus": "flagged",
    "startDateTime": { "dateTime": "2026-10-21T09:00:00.0000000", "timeZone": "UTC" },
    "dueDateTime":   { "dateTime": "2026-10-23T17:00:00.0000000", "timeZone": "UTC" } } }
```
Complete (ILLUSTRATIVE, **UNVERIFIED** whether `completedDateTime` is needed/accepted):

```json
{ "flag": { "flagStatus": "complete", "completedDateTime": { "dateTime": "2026-10-20T12:00:00.0000000", "timeZone": "UTC" } } }
```
Clear: `{ "flag": { "flagStatus": "notFlagged" } }` (ILLUSTRATIVE; `notFlagged` is a verified enum value; that it clears start/due is **UNVERIFIED**).

Response (ILLUSTRATIVE, shape per VERIFIED message resource):

```http
HTTP/1.1 200 OK
Content-type: application/json

{ "@odata.etag":"W/\"...\"", "id":"AAMk...", "changeKey":"...", "flag": { "flagStatus": "flagged" }, "...": "full message ..." }
```
The PATCH response is the full message (VERIFIED: "updated message object"), including an HTML `body`. Whether `$select` limits it is **UNVERIFIED**.

### Implementation notes for Slow Mail

1. Flag = `PATCH` with only `{"flag":{...}}` (do not send unchanged fields - Microsoft: "For best performance you shouldn't include existing values that haven't changed").
2. Optimistic UI: set local `flagStatus`, send PATCH, roll back on non-2xx. No `If-Match`; last-writer-wins.
3. Ignore the response body except `id`, `changeKey`, `flag`, `lastModifiedDateTime` (it can be large).
4. Never send `dueDateTime` without `startDateTime` (400). If the UI only has a due date, send `startDateTime` = same instant/date.
5. Unflag = `notFlagged`; mark done = `complete`. On `complete`, send `completedDateTime` (harmless if ignored; UNVERIFIED either way) - verify on real tenant.
6. Delta will echo your own change (message appears as updated). Merge idempotently.
7. Flag changes by other clients arrive in delta as updates with `flag` included (assumed; **UNVERIFIED** that `flag` changes trigger a delta entry and that `flag` is selectable in delta - test in a real mailbox before depending on it).

---

## 6. Message body as text, bodyPreview, attachments

**Status: VERIFIED** for the Prefer header, body shape, `Preference-Applied`, bodyPreview length, attachment types and properties, `hasAttachments` caveat, `$value` 405 for reference attachments. **SNIPPET/UNVERIFIED**: `$select` on the attachments collection (generic support stated; no collection-specific sample read), contentBytes-by-default behaviour for itemAttachment/reference, text-conversion edge cases.

**Sources (retrieved 2026-10-20):** GDC `api-reference/v1.0/api/message-get.md` (ms.date 2024-06-21) - https://learn.microsoft.com/graph/api/message-get ; `resources/message.md`; `resources/itembody.md`; `api/message-list-attachments.md` (ms.date 2024-06-21) - https://learn.microsoft.com/graph/api/message-list-attachments ; `api/attachment-get.md`; `resources/attachment.md`, `fileattachment.md`, `itemattachment.md`, `referenceattachment.md` ; `concepts/includes/known-issues/query-parameters.md` (ms.date 2026-02-25). Permissions: get message `Mail.ReadBasic`/`Mail.Read` (VERIFIED; `Mail.ReadWrite` is higher so fine; note ReadBasic cannot read body).

### 6.1 Get body as text (COPIED)

```http
GET https://graph.microsoft.com/v1.0/me/messages/AAMkAGVm...AAA=/?$select=subject,body,bodyPreview,uniqueBody
Prefer: outlook.body-content-type="text"
```
Header (VERIFIED): "The format of the **body** and **uniqueBody** properties to be returned in. Values can be "text" or "html". A `Preference-Applied` header is returned as confirmation... If the header is not specified, the **body** and **uniqueBody** properties are returned in HTML format."

Response (COPIED, shortened):

```http
HTTP/1.1 200 OK
Content-type: application/json
Preference-Applied: outlook.body-content-type="text"

{
    "@odata.context":"https://graph.microsoft.com/v1.0/$metadata#users('cd209b0b-3f83-4c35-82d2-d88a61820480')/messages(subject,body,bodyPreview,uniqueBody)/$entity",
    "@odata.etag":"W/\"CQAAABYAAABmWdbhEgBXTophjCWt81m9AAAoZYj4\"",
    "id":"AAMkAGI1AAAoZCfHAAA=",
    "subject":"Welcome to our group!",
    "bodyPreview":"Welcome to our group, Dana! Hope you will enjoy working with us !\r\n\r\nWould you like to choose ...",
    "body":{
        "contentType":"text",
        "content":"Welcome to our group, Dana! Hope you will enjoy working with us [😊] [😊] ... !\r\n\r\nWould you like to choose a day ...\r\n\r\nThanks!\r\n\r\n"
    },
    "uniqueBody":{ "contentType":"text", "content":"..." }
}
```
Observations from the verified sample: text bodies use `\r\n` line endings; emoji are rendered as bracketed placeholders (`[😊]` in the JSON, i.e. `[😊]`) in the text conversion; `@odata.context` embeds the user GUID.

`itemBody` (VERIFIED): `{ "content": "string", "contentType": "String" }` with contentType `text` or `html`. **`bodyPreview`** (VERIFIED): "The first 255 characters of the message body. It is in text format." `uniqueBody`: not returned by default; only via `$select`.

If you ask for text and Outlook cannot convert, the `contentType` returned is authoritative - always read `body.contentType`, do not assume the header worked (a `Preference-Applied` header confirms it; absence is **UNVERIFIED** behaviour).

### 6.2 List attachments (metadata only)

Request (VERIFIED endpoint; `$select` ILLUSTRATIVE per the generic "supports the OData Query Parameters" statement):

```http
GET https://graph.microsoft.com/v1.0/me/messages/{id}/attachments?$select=id,name,contentType,size,isInline
Prefer: IdType="ImmutableId"
```
**Do not omit `$select`.** The only sample (VERIFIED, COPIED) shows the default list includes `contentBytes` for a file attachment:

```json
{
  "value": [
    {
      "@odata.type": "microsoft.graph.fileAttachment",
      "contentType": "contentType-value",
      "contentLocation": "contentLocation-value",
      "contentBytes": "contentBytes-value",
      "contentId": "null",
      "lastModifiedDateTime": "datetime-value",
      "id": "id-value",
      "isInline": false,
      "name": "name-value",
      "size": 99
    }
  ]
}
```
(In the doc the `@odata.type` lacks the leading `#`; the real service prefixes `#microsoft.graph...` - normalise by stripping a leading `#`. Real prefix is **UNVERIFIED** here but is how the move/delta samples write message types: `"#microsoft.graph.message"`.)

ILLUSTRATIVE response for the `$select` above:

```json
{
  "@odata.context": "https://graph.microsoft.com/v1.0/$metadata#users('...')/messages('...')/attachments(id,name,contentType,size,isInline)",
  "value": [
    { "@odata.type": "#microsoft.graph.fileAttachment",      "id": "AAMk...1", "name": "q3.pdf",     "contentType": "application/pdf", "size": 183421, "isInline": false },
    { "@odata.type": "#microsoft.graph.itemAttachment",      "id": "AAMk...2", "name": "Re: budget", "contentType": null,              "size": 2411,   "isInline": false },
    { "@odata.type": "#microsoft.graph.referenceAttachment", "id": "AAMk...3", "name": "plan.docx",  "contentType": null,              "size": 745,    "isInline": false }
  ]
}
```
Verified type facts:
- Base `attachment` properties: `contentType`, `id`, `isInline`, `lastModifiedDateTime`, `name`, `size` (all exist on all three types).
- **fileAttachment** adds `contentBytes` (base64), `contentId`, `contentLocation` ("Don't use this property as it isn't supported"). `size` = bytes of the attachment.
- **itemAttachment** (contact/event/message): navigation property `item` (not expanded unless `$expand`); `contentType` "Returned as `null` by default, when not set explicitly".
- **referenceAttachment** (link to cloud file): `contentType` null by default; `size` is "the size of the metadata that is stored on the message ... doesn't indicate the size of the actual file"; **`GET .../$value` returns HTTP 405**.
- `isInline`: inline images; and `hasAttachments` on the message "doesn't include inline attachments, so if a message contains only inline attachments, this property is false" (VERIFIED).
- `$filter` on `/attachments` is **ignored** if present (VERIFIED, known-issues query-parameters).
- Raw bytes: `GET .../attachments/{id}/$value` returns raw file/item content (VERIFIED) and `GET .../messages/{id}/$value` returns the whole MIME message (VERIFIED). Upload sizes: <3 MB direct, 3 to 150 MB via upload session (VERIFIED) - relevant only for later send-with-attachment.

### Implementation notes for Slow Mail

1. List view: from delta, take `bodyPreview` (255 chars, text) + `hasAttachments`. Do **not** fetch bodies for the list.
2. Reading a message: one `GET /me/messages/{id}?$select=id,subject,from,toRecipients,ccRecipients,receivedDateTime,body,uniqueBody,hasAttachments,isRead,flag,internetMessageId,parentFolderId,changeKey` with `Prefer: outlook.body-content-type="text"`, `IdType="ImmutableId"`. Render `body.content` as plain text (never HTML into a terminal/UI); check `body.contentType == "text"`.
3. Attachments: call the list only when `hasAttachments` is true, **or** when the body text contains `[cid:` markers (for inline-only). Always use the exact `$select=id,name,contentType,size,isInline`. Show metadata (name, type, human-readable size). **Never** request `contentBytes`, `/$value`, or `$expand=item` automatically; downloading is an explicit future action with a size cap.
4. Parse attachment `@odata.type` by stripping an optional leading `#`. Unknown types are shown as "attachment" with name/size.
5. For `referenceAttachment`, never call `$value` (405). Show "Link: name".
6. `size` is `Int32`; `0` or missing must not crash the UI.
7. `body.content` can be large (limit **UNVERIFIED**); cap the in-memory read with `CURLOPT_MAXFILESIZE`-style guards/streaming limit, say 5 MB, and truncate for display.

---

## 7. Mail folder lookup: `GET /me/mailFolders/{wellKnownName}`

**Status: VERIFIED** (endpoint, permissions, response fields). **UNVERIFIED**: well-known name in the *path* for `/me/mailFolders/{name}` is demonstrated in the resource page for `drafts` (VERIFIED example); that `archive` works the same way when the folder is absent.

**Sources (retrieved 2026-10-20):** GDC `api-reference/v1.0/api/mailfolder-get.md` (ms.date 2024-04-04) - https://learn.microsoft.com/graph/api/mailfolder-get ; `resources/mailfolder.md` (ms.date 2024-08-23). Permission: delegated `Mail.ReadBasic` least; `Mail.ReadWrite`, `Mail.Read` higher (VERIFIED).

### 7.1 Request (VERIFIED example form from `mailfolder.md`: `GET /me/mailFolders/drafts`)

```http
GET https://graph.microsoft.com/v1.0/me/mailFolders/inbox
Authorization: Bearer {token}
```
(Replace with `archive`, `deleteditems`.)

### 7.2 Response (COPIED)

```http
HTTP/1.1 200 OK
Content-type: application/json

{
  "id": "AAMkAGVmMDEzM",
  "displayName": "Inbox",
  "parentFolderId": "AAMkAGVmMDEzI",
  "childFolderCount": 2,
  "unreadItemCount": 59,
  "totalItemCount": 60,
  "isHidden": false
}
```
Properties (VERIFIED): `id`, `displayName`, `parentFolderId`, `childFolderCount`, `totalItemCount`, `unreadItemCount`, `isHidden`. A second verified sample (Sent Items) also contains `"sizeInBytes": 53857` although the properties table omits it - treat `sizeInBytes` as optional. `displayName` is **localised** (well-known names are not). `totalItemCount`/`unreadItemCount` "include items in a mail folder irrespective of their item types" (e.g. meeting requests), so they can differ from the number of `message` rows in delta. `isHidden` default false; hidden folders appear in list only with `includeHiddenFolders=true`.

Folder ids are constant (VERIFIED: "their regular IDs were already constant").

### Implementation notes for Slow Mail

1. After sign-in resolve: `inbox`, `archive`, `deleteditems` (and optionally `sentitems`, `drafts`, `junkemail`). Cache `{wellKnownName -> id, displayName, parentFolderId}`; re-resolve on a 404 from any folder-scoped call and at most once per day.
2. Use `$select=id,displayName,parentFolderId,totalItemCount,unreadItemCount` to keep responses small.
3. Inbox badge counts: use `unreadItemCount` from the folder resource (VERIFIED guidance: avoids the slow `$count`+`$filter=isread eq false` query).
4. A 404 on `archive` is a first-class state (section 4 note 2).
5. Never infer a folder's role from `displayName` (localised, user-renamable).

---

## 8. Throttling, retries, 401/404/409/412

**Status: VERIFIED** for 429 shape, Retry-After guidance, Outlook limits (10,000/10 min, 4 concurrent, 150 MB/5 min), JSON batching concurrency, HTTP status table. **SNIPPET** for MailboxConcurrency `Retry-After: 1` semantics and `ApplicationThrottled` code, 401 error JSON. **UNVERIFIED** for Outlook-specific 404/409/412 error codes.

**Sources (retrieved 2026-10-20):** GDC `concepts/throttling.md` (ms.date 2025-01-14) - https://learn.microsoft.com/graph/throttling ; GDC `concepts/throttling-limits.md` (ms.date 2025-01-14) - https://learn.microsoft.com/graph/throttling-limits ; GDC `includes/throttling-outlook.md` (the Outlook limits block included in that page); GDC `concepts/errors.md` (ms.date 2024-11-07) - https://learn.microsoft.com/graph/errors ; Q&A "Microsoft graph batch API concurrent request reduction to 4..." and the 2019 devblog "Throttling changes coming to beta Outlook APIs ..." (SNIPPET only; the pages were blocked).

### 8.1 429 (VERIFIED, COPIED)

```http
HTTP/1.1 429 Too Many Requests
Content-Length: 312
Content-Type: application/json
Retry-After: 10

{
  "error": {
    "code": "TooManyRequests",
    "innerError": {
      "code": "429",
      "date": "2020-08-18T12:51:51",
      "message": "Please retry after",
      "request-id": "94fb3b52-452a-4535-a601-69e0a90e3aa2",
      "status": "429"
    },
    "message": "Please retry again later."
  }
}
```
Verified guidance: use 429 to detect throttling; the failed response includes `Retry-After`; "Wait the number of seconds specified"; retry; if 429 again "continue to use the recommended `Retry-After` delay"; "Avoid immediate retries, because all requests accrue against your usage limits"; "If no `Retry-After` header is provided by the response, we recommend implementing an exponential backoff retry policy". Throttling "depends on the type and number of requests ... writes are throttled but reads are still permitted".

### 8.2 Outlook service limits (VERIFIED, `includes/throttling-outlook.md`)

"The Outlook service applies limits to each app ID and mailbox combination."

| Limit | Applies to |
|---|---|
| 10,000 API requests in a 10-minute period | v1.0 and beta |
| **Four concurrent requests** | v1.0 and beta |
| 150 MB upload (PATCH, POST, PUT) in a 5-minute period | v1.0 and beta |

Global limit (VERIFIED): 130,000 requests per 10 seconds per app across all tenants. "The first limit to be reached triggers throttling behavior." Limits "are subject to change".

JSON batching (VERIFIED): "by default, Microsoft Graph sends the Outlook service up to four individual requests from the batch at a time"; the batch HTTP response itself is `200` while individual items can be `429`; "retry each failed request ... using the value provided in the `retry-after` response header from the JSON content".

**Concurrency throttle semantics (SNIPPET):** a Microsoft Q&A answer says the `MailboxConcurrency` limit "is not time-based": the 429 carries `Retry-After: 1` and the code `ApplicationThrottled` with a message like "Application is over its MailboxConcurrency limit", and a retry only succeeds once at least one in-flight call finishes. ILLUSTRATIVE (code string from SNIPPET, message wording **UNVERIFIED**):

```http
HTTP/1.1 429 Too Many Requests
Retry-After: 1
Content-Type: application/json

{"error":{"code":"ApplicationThrottled","message":"Application is over its MailboxConcurrency limit.","innerError":{"date":"2026-10-20T10:00:00","request-id":"...","client-request-id":"..."}}}
```

### 8.3 503 and other statuses

`errors.md` (VERIFIED): `503 Service Unavailable` = "temporarily unavailable for maintenance or is overloaded. You can repeat the request after a delay, the length of which can be specified in a Retry-After header." 504 = gateway timeout; 509 = bandwidth limit exceeded; 500/501 = server errors; 401 = "Required authentication information is either missing or not valid"; 403 includes CA `insufficient_claims`; 404 = "The requested resource doesn't exist"; 409 = conflict ("For a Directory_ConcurrencyViolation exception, you can repeat the request after some delay... If a Retry-After header is present, that value can be used"); 410 = gone; **412 Precondition Failed** = "A precondition provided in the request (such as an if-match header) doesn't match the resource's current state"; 413, 415, 422, 423.

Error body (VERIFIED): `{"error":{"code":"string","message":"string","innererror":{"code":"string"},"details":[]}}`. Rules: code against `code` (and nested `innerError.code`), never `message`. The 429 sample spells it `innerError`; the schema page spells `innererror`; **parse case-insensitively**.

Zammad report (SNIPPET): some 503s come from a front proxy as **HTML with no Retry-After and no Graph JSON**. Do not assume the body is JSON.

### 8.4 401 token expired

Rule: 401 = "Required authentication information is either missing or not valid" (VERIFIED). Typical expired-token body (SNIPPET from community posts; not an official sample):

```json
{"error":{"code":"InvalidAuthenticationToken","message":"Lifetime validation failed, the token is expired.","innerError":{"date":"2024-03-27T03:42:52","request-id":"bd293dcf-...","client-request-id":"bd293dcf-..."}}}
```
Variant message "Access token has expired or is not yet valid." also reported. The `message` varies; only branch on HTTP 401 / `code == InvalidAuthenticationToken` (that code string is SNIPPET). `WWW-Authenticate` header content is **UNVERIFIED** here.

### 8.5 404 `ErrorItemNotFound`

Moved/deleted by another client: Graph returns 404 with code `ErrorItemNotFound` and message like "The specified object was not found in the store." - code **SNIPPET** (seen in a Java-SDK issue), exact behaviour for a message id that was moved with mutable ids (old id becomes invalid) is inferred (**UNVERIFIED**). With immutable ids a moved message keeps its id, so a 404 then more likely means truly deleted. ILLUSTRATIVE:

```http
HTTP/1.1 404 Not Found
{"error":{"code":"ErrorItemNotFound","message":"The specified object was not found in the store.","innerError":{"date":"2026-10-20T10:00:00","request-id":"...","client-request-id":"..."}}}
```

### 8.6 409 / 412 concurrency

Status meanings verified generically (above). Outlook-specific code names (e.g. `ErrorIrresolvableConflict`, `ErrorInvalidChangeKey`) are **UNVERIFIED** (search surfaced only Exchange/EWS-era sources, SNIPPET). For `PATCH /messages/{id}` the doc defines no `If-Match` precondition, so 412 should be rare for Slow Mail; 409 may occur on concurrent writes to the same item.

### Implementation notes for Slow Mail

1. One central request function: `send(request)` -> classifies the result into: `Ok`, `AuthExpired(401)`, `Forbidden(403)`, `NotFound(404)`, `Gone(410)`, `Throttled(429|503)`, `Conflict(409|412)`, `Transient(5xx/network)`, `Fatal`.
2. **Concurrency gate:** a counting semaphore of **3** (below the documented 4; per app+mailbox, so only Slow Mail's own calls count). Serialise delta paging; run moves/patches through the gate. Never fire a burst of N parallel `PATCH`/`move` calls.
3. **Rate budget:** keep well under 10,000 per 10 min (~16/s). A slow triage client will naturally be far below. Add a token-bucket (e.g. 5 req/s sustained) as a safety net.
4. **429/503 handling:** read `Retry-After` (integer seconds; also accept an HTTP-date form defensively - **UNVERIFIED** that Graph ever sends a date). Sleep exactly that long (min 1 s), then retry the **same** request. If no `Retry-After`: exponential backoff with jitter, base 1 s, factor 2, cap 60 s. Max 6 attempts for reads, 4 for writes, then surface "Mail service is busy" and keep the pending action queued (do not drop user intent).
5. While one 429 is in flight, **pause the whole gate** until the `Retry-After` elapses (Graph keeps counting resource use during throttling - VERIFIED).
6. **401:** refresh the access token once (section 1.6) and retry the request once. If the refresh fails with `invalid_grant`/`interaction_required`/`AADSTS530036`, move to "signed out - sign in again". Do not loop. Do not treat 401 as a throttling signal.
7. **403:** if body/header indicates `insufficient_claims`, a Conditional Access claims challenge is needed (re-auth interactively); otherwise report "permission/licence issue" (no retry).
8. **404 on a message call:** the message was moved or deleted elsewhere. Remove/refresh the local row: `GET` its current state if you hold an immutable id (200 -> update `parentFolderId`; 404 -> delete locally), or simply trigger a delta run for the folder. Show non-fatal UI feedback ("Message is no longer in the Inbox").
9. **404 on a folder call:** re-resolve well-known folders (section 7).
10. **409/412:** refetch the message (`GET ?$select=id,changeKey,flag,isRead,parentFolderId`), reapply the user's intended change once, then give up with a visible error.
11. **Delta URLs:** see section 2.7 - 4xx other than 401/403/429 = resync.
12. Idempotency: queued writes must be safe to retry after an unknown outcome (timeout): flag/read PATCH are idempotent; move is not (see section 4 note 5).
13. Log `request-id` and `client-request-id` from errors (they are what Microsoft Support asks for). Send your own `client-request-id` GUID header per request (**UNVERIFIED** here but standard Graph practice) to correlate.
14. libcurl: set connect/total timeouts; treat `CURLE_OPERATION_TIMEDOUT`/connection resets as `Transient` with backoff; read headers case-insensitively (`Retry-After`, `retry-after`).

---

## 9. Later phase: reply / forward (and `Mail.Send`)

**Status: VERIFIED** for endpoints, JSON bodies, 202 responses, 400 rules, Sent Items behaviour, permission `Mail.Send`. **UNVERIFIED**: behaviour with `Mail.ReadWrite` only (it fails; the docs list `Mail.Send` as the only permission), draft-then-send flow details beyond the quoted docs.

**Sources (retrieved 2026-10-20):** GDC `api-reference/v1.0/api/message-reply.md` (ms.date 2024-04-04) - https://learn.microsoft.com/graph/api/message-reply ; `api/message-forward.md` (ms.date 2024-06-21) - https://learn.microsoft.com/graph/api/message-forward ; `api/message-send.md`; `api/message-createreply.md`; `concepts/outlook-immutable-id.md` ("Immutable ID with sending mail"); `known-issues/mail.md`. Permissions: reply and forward - delegated `Mail.Send` only, "Higher privileged: Not available" (VERIFIED, includes `message-reply-permissions.md`, `message-forward-permissions.md`).

### 9.1 Reply (COPIED)

```http
POST https://graph.microsoft.com/v1.0/me/messages/AAMkADA1MTAAAAqldOAAA=/reply
Content-Type: application/json

{
  "message":{
    "toRecipients":[
      { "emailAddress": { "address":"samanthab@contoso.com", "name":"Samantha Booth" } },
      { "emailAddress": { "address":"randiw@contoso.com", "name":"Randi Welch" } }
     ]
  },
  "comment": "Samantha, Randi, would you name the group please?"
}
```
```http
HTTP/1.1 202 Accepted
```
Verified rules: body parameters are `comment` (string, "Can be an empty string") and `message` ("Any writeable properties to update in the reply message"). "Specify either a comment or the **body** property of the `message` parameter. Specifying both will return an HTTP `400 Bad Request` error." If the original has `replyTo`, the reply goes to `replyTo`, not `from`. "This method saves the message in the **Sent Items** folder." Header `Prefer: outlook.timezone` (optional) controls the "Sent" field timezone in the generated HTML. MIME alternative: `Content-Type: text/plain` with a base64 MIME body; malformed MIME -> 400 with "Invalid base64 string for MIME content".

### 9.2 Forward (COPIED)

```http
POST https://graph.microsoft.com/v1.0/me/messages/{id}/forward
Content-type: application/json

{
  "comment": "comment-value",
  "toRecipients": [
    { "emailAddress": { "name": "name-value", "address": "address-value" } }
  ]
}
```
`202 Accepted`, no body. Rules (VERIFIED): comment vs `message.body` mutually exclusive (400); recipients must be given **either** as top-level `toRecipients` **or** in `message.toRecipients` - "Specifying both or specifying neither will return an HTTP 400 Bad Request error". Malformed MIME: 400, code `ErrorMimeContentInvalidBase64String`, message "Invalid base64 string for MIME content." (COPIED).

### 9.3 Draft route and scopes

Create draft + `POST /me/messages/{id}/send` (202; needs `Content-Length: 0`; saves to Sent Items) (VERIFIED). Draft creation requires `Mail.ReadWrite` (UNVERIFIED here), sending requires `Mail.Send`. Known issue (VERIFIED, 2026-02-25): the `comment` parameter on `createReply`/`createReplyAll`/`createForward` "isn't part of the body of the response message draft".

### Implementation notes for Slow Mail

1. `Mail.Send` is not requested in phase 1. When reply/forward ships: re-run interactive sign-in requesting `offline_access User.Read Mail.ReadWrite Mail.Send` (section 1.7: refresh cannot add scopes), replace stored tokens.
2. v1 reply UI: plain text only -> `{"comment": "<text>"}` (no `message`). Remember `comment` XOR `message.body`.
3. A 202 means accepted, not delivered. The copy appears in Sent Items later (immutable-ID doc: may not exist immediately). Do not poll aggressively; the next Sent Items delta shows it (if tracking Sent Items).
4. Confirm-before-send is a product requirement, not an API one: Graph sends instantly with no undo.
5. Recipient cap: 500 total recipients per message (VERIFIED in `message.md`).
6. Replying to a message you also flag/move: sequence the calls (gate of 3) and handle idempotency: never auto-retry a `reply`/`forward` POST after an ambiguous failure (timeout) without asking - risk of duplicate send.

---

## 10. 2025-2026 changes affecting this app

**Status mixed.** What I could and could not check:

| Item | Status | Evidence / impact |
|---|---|---|
| Message delta `@removed` for moved-out + read/unread changes "not filtered" note | VERIFIED (in `message-delta.md` as of retrieval) | Section 2; already designed in |
| Known issue: delta + immutable ids may miss moved-out-then-back events | VERIFIED (`known-issues/mail.md`, ms.date 2026-02-25) | Section 2.6/3; add reconcile |
| Delta `changeType` (created/updated/deleted) and "$filter returns up to 5,000 messages" | VERIFIED (concept doc ms.date 2024-11-07) | Section 2 |
| Throttling docs last updated 2025-01-14; Outlook limits unchanged in that revision (10,000/10 min, 4 concurrent) | VERIFIED | Section 8 |
| Conditional Access "Authentication flows" (device code block), `ms.date` 2026-03-24, Microsoft recommends blocking; protocol tracking and `AADSTS530036` | VERIFIED | Section 1.8: biggest risk to device-code sign-in |
| Authentication-flows policies enforced on Device Registration Service since Sep 2024 | VERIFIED | Not relevant to us |
| "What's new in Microsoft Graph" (`ms.date` 2026-10-05) covers only **Sep-Oct 2026**. Mail items there: message-trace `recalled` status (GA), mailboxFolder `isHidden` (beta), calendar delta. **None change `message` v1.0, move, delta, flag or attachments.** | VERIFIED (for Sep-Oct 2026 only) | No action. I did **not** find a changelog covering earlier 2025-2026 months (the file in `main` holds only two months) - **gap** |
| `message-delta` page (ms.date 2024-06-21) still lists `Prefer: odata.maxpagesize` only | VERIFIED | No documented page-size cap |
| Exchange Web Services (EWS) retirement in Exchange Online: phased disablement from **2026-10-01**, permanent shutdown from **2027-04-01** (one outlet reads it as May 2027); Graph is the replacement; Graph gaps remain e.g. archive mailboxes | SNIPPET (Microsoft Message Center MC1227454, reproduced by third parties; I could not open the primary) | Slow Mail never used EWS. Side-effects: tenants will tighten Exchange app access in Oct 2026; Graph gaps confirm "Online Archive not reachable" (section 4). Check for a related DevBlog (June 2026, "migrating EWS notifications to Microsoft Graph" - title only, SNIPPET) |
| Outlook REST API v2.0 (`outlook.office.com/api/v2.0`) retirement; third-party guide claims decommissioned 2024-03-31 returning 410 | SNIPPET (vendor blog) | Slow Mail uses `graph.microsoft.com/v1.0` only; never use `outlook.office.com/api`. Do not mix in tokens for the old audience |
| Outlook delta tokens expire by cache eviction (no fixed lifetime) | VERIFIED (`delta-query-overview.md`) | Section 2.7 |
| Concept overview page says mail flagging "is currently in preview" | VERIFIED stale text | Rely on v1.0 `message-update`/`followupFlag` pages |
| `slow_down` for Entra device code | not documented | Section 1.4 |

**Gap statement:** I could not verify via primary source any 2025 change to Graph mail (the docs repo only exposed Sep-Oct 2026 what's-new). Before shipping, check the Microsoft Graph changelog (https://developer.microsoft.com/graph/changelog) and Exchange Team blog for Outlook/Graph mail changes Jan 2025 - Aug 2026.

---

## 11. Fake-server fidelity checklist

The local fake Graph server (and fake Entra) must emulate **at least** the following, so contract tests would fail against a wrong client. Items marked (V)=verified from docs; (U)=assumption/unverified - emulate but flag it in test names as "assumed behaviour"; make assumed behaviours switchable.

### 11.1 Entra device-code endpoints

- [ ] `POST /{tenant}/oauth2/v2.0/devicecode`: form-encoded `client_id`, `scope`. Returns `device_code`, `user_code`, `verification_uri`, `expires_in` (default 900), `interval`, `message`; **no** `verification_uri_complete` (V). 400 on missing `client_id`/`scope`.
- [ ] `POST /{tenant}/oauth2/v2.0/token` with `grant_type=urn:ietf:params:oauth:grant-type:device_code` (V): requires matching `client_id`; errors `authorization_pending`, `authorization_declined`, `bad_verification_code`, `expired_token` (V) as HTTP 400 with `error`, `error_description`, `error_codes`, `timestamp`, `trace_id`, `correlation_id` (V shape). Also an optional `slow_down` mode (U) - verifies the client adds >=5 s.
- [ ] Polling too fast (< `interval`) -> `slow_down` in strict mode (U).
- [ ] Success returns `token_type: Bearer`, `scope` (space-separated, mixed-case), `expires_in` (3599), `access_token`, `refresh_token` **only if `offline_access` requested**, `id_token` **only if `openid` requested** (V).
- [ ] If the client sends `client_secret`, accept; if the app is configured as "confidential" and no secret is sent -> `invalid_client`/`AADSTS7000218` (V text) to test the "Allow public client flows" misconfiguration path.
- [ ] `grant_type=refresh_token`: returns new access token **and a new refresh token**; invalidates the old refresh token in strict mode (V: "expected to discard the old"). `scope` must be subset of granted, else `invalid_grant`/`invalid_scope` (V rule; code U).
- [ ] CA-block mode: `devicecode`/`token` returns an error for device flow (AADSTS-style, U body) and a refresh failing with `AADSTS530036` (V text) -> client must go to "sign in again".
- [ ] Access token expiry: tokens with short TTL; Graph returns 401 once expired.

### 11.2 Graph auth/headers

- [ ] 401 `InvalidAuthenticationToken` on missing/expired token, with `error.innerError.date/request-id/client-request-id` (code string SNIPPET).
- [ ] Records the `Prefer` header(s) per request; supports `odata.maxpagesize`, `IdType="ImmutableId"`, `outlook.body-content-type="text"` both as separate `Prefer:` lines and comma-joined (U). Returns `Preference-Applied: outlook.body-content-type="text"` (V) when applied.
- [ ] Response includes `request-id` and `client-request-id` headers (U) and echoes error `innerError`.

### 11.3 Folders

- [ ] `GET /me/mailFolders/{wellKnown|id}` for `inbox`, `archive`, `deleteditems` (+ `sentitems`, `drafts`, `junkemail`) returning `id`, `displayName` (localisable), `parentFolderId`, `childFolderCount`, `unreadItemCount`, `totalItemCount`, `isHidden` (V); `sizeInBytes` optional (V).
- [ ] Configurable "mailbox without archive folder": `GET /me/mailFolders/archive` -> 404 `ErrorItemNotFound` (code SNIPPET; scenario U).
- [ ] Well-known names lowercase; unknown name -> 404.
- [ ] Folder ids are constant for the life of the folder (V).

### 11.4 Delta

- [ ] `GET /me/mailFolders/{id}/messages/delta`; folder id in returned links is rendered as `mailFolders('<id>')` (V sample form). Accept the well-known name too (U).
- [ ] Honour `$select` (always include `id`, plus `@odata.type`, `@odata.etag` (V)), `$top`, `$filter=receivedDateTime ge|gt <dt>` only (anything else -> 400, V), `$orderby=receivedDateTime desc` only, `changeType=created|updated|deleted` (V), `Prefer: odata.maxpagesize`. Reject `$search`.
- [ ] Initial request returns pages with `@odata.nextLink` (`$skiptoken`) until a final page with `@odata.deltaLink` (`$deltatoken`); never both in one page; **empty intermediate pages allowed** (V). Links are returned in the **body**, absolute URLs (V).
- [ ] Token encodes `$select/$filter/changeType/$orderby`; subsequent requests via the link need no parameters; the server must **ignore/refuse** conflicting re-specified params (V statement that they are encoded).
- [ ] Round 2 (`$deltatoken`) returns only changes since: new -> full select set; updated -> `id` + **changed props only** (V: "at least the updated properties") - a strict mode returns only changed props; deleted -> `{"@odata.type":"#microsoft.graph.message","id":..., "@removed":{"reason":"deleted"}}` (V).
- [ ] **Moved-out message** -> `@removed` `reason: "deleted"` in source folder delta (V). Moved-in -> appears as normal entry in destination folder delta.
- [ ] With `$filter` in the initial request: caps the result at 5,000 messages (V); `@removed` and read/unread-change events are returned even for messages outside the filter (V).
- [ ] Without `$orderby`, shuffle order within the round in strict mode (V: not guaranteed); duplicates/replays: optionally emit the same entity twice in one round (V).
- [ ] Immutable-ID gap mode (V known issue): message moved out then back in -> second delta shows the `@removed` only, no re-add. Without ImmutableId: removed old id + new id.
- [ ] Token expiry modes: (i) `400 syncStateNotFound`, (ii) `410 Gone` + `Location` with empty `$deltatoken` (V behaviour; exact codes U). Evict tokens after N rounds ("cache" semantics, V).
- [ ] Eventual consistency mode: a change appears in delta one poll late (V).
- [ ] `isRead` is boolean (do not copy the doc's string typo).

### 11.5 Messages and IDs

- [ ] `GET /me/messages/{id}` returns the full resource (V sample fields: `id`, `createdDateTime`, `lastModifiedDateTime`, `changeKey`, `categories`, `receivedDateTime`, `sentDateTime`, `hasAttachments`, `internetMessageId`, `subject`, `bodyPreview`, `importance`, `parentFolderId`, `conversationId`, `isDeliveryReceiptRequested`, `isReadReceiptRequested`, `isRead`, `isDraft`, `webLink`, `inferenceClassification`, `body`, `sender`, `from`, `toRecipients`, `ccRecipients`, `bccRecipients`, `replyTo`, `flag`).
- [ ] `body` is HTML unless `Prefer: outlook.body-content-type="text"` -> `body.contentType:"text"`, `\r\n` newlines, emoji as `[<emoji>]`, header `Preference-Applied` (V). `uniqueBody` only via `$select`.
- [ ] `bodyPreview` = first **255** characters of the text body (V).
- [ ] `hasAttachments` false if only inline attachments (V).
- [ ] **ID model switch:** default mode - move changes the message `id` (old id then 404 `ErrorItemNotFound`); `Prefer: IdType="ImmutableId"` mode - id stable across folder moves in the same mailbox (V); ids case-sensitive (V). The header applies per request: a request without it sees the mutable id form (strict mode) (U). Simulate "archive-mailbox move / export-reimport changes the id" (V).
- [ ] `POST /me/translateExchangeIds` (V): up to 1,000 ids, returns `{ "value": [{ "targetId", "sourceId" }] }`.

### 11.6 Move / flag

- [ ] `POST /me/messages/{id}/move` with JSON `destinationId` (id or well-known name): response **201 Created** with the full moved message, `parentFolderId` = destination (V); new `id` in mutable mode; same `id` in immutable mode (V). Missing/unknown `destinationId` -> 400/404 (U). Unknown/moved source id -> 404 `ErrorItemNotFound` (code SNIPPET).
- [ ] `PATCH /me/messages/{id}` with `{"flag": {...}}`: **200** + updated message (V). Validate `flagStatus` in `notFlagged|complete|flagged` (V). `dueDateTime` without `startDateTime` -> **400** (V). `dateTimeTimeZone` shape `{dateTime,timeZone}` (V). Clearing sets `flagStatus: notFlagged` (U: also clears dates). Bumps `changeKey`/`lastModifiedDateTime`/`@odata.etag`. No `If-Match` required (V absence); optional 412 mode when `If-Match` given and stale (U).
- [ ] PATCH of read-only/draft-only fields on a non-draft (`subject`, `body`, recipients) -> error (V rule: updatable only if `isDraft`).
- [ ] `PATCH {"isRead": true}` updates and later appears in delta (V: read/unread events).
- [ ] Mutations by "another client" (test hook) to simulate 404 on stale ids, flag changes, and moves.

### 11.7 Attachments

- [ ] `GET /me/messages/{id}/attachments` default returns **`contentBytes`** for file attachments (V); with `$select=id,name,contentType,size,isInline` returns only those (+`@odata.type`, `@odata.context`) (U for the collection; generic V).
- [ ] Three types: `#microsoft.graph.fileAttachment`, `#microsoft.graph.itemAttachment`, `#microsoft.graph.referenceAttachment`; `itemAttachment`/`referenceAttachment` `contentType` null by default (V). Emit `@odata.type` both with and without `#` in a "doc-style" mode to test normalisation.
- [ ] `$filter` on `/attachments` is **ignored** (V). `GET .../attachments/{id}/$value` of a referenceAttachment -> **405** (V). A test should **fail the client** if it ever requests `$value` or `contentBytes` (record these as violations).
- [ ] `size` is `Int32`; referenceAttachment `size` is metadata size (V).

### 11.8 Throttling and errors

- [ ] Per (app, mailbox) limits: **4 concurrent** requests (V) -> 5th concurrent returns `429` `Retry-After: 1`, code `ApplicationThrottled` (code SNIPPET). 10,000 requests per 10 min (V) -> 429 with `Retry-After: N` (V shape from sample: `TooManyRequests`, `innerError.code:"429"`). 
- [ ] 429/503 with `Retry-After` (seconds); a variant 429 **without** `Retry-After` (V: client must use exponential backoff); a variant 503 with an **HTML body and no Retry-After** (SNIPPET).
- [ ] Requests made during throttle window while the client retries immediately extend the window (V: "all requests accrue against your usage limits").
- [ ] JSON `$batch` (if ever used): outer 200, per-item 429 with `retry-after` header inside the item (V).
- [ ] 401 expiry mid-session; 403 `insufficient_claims` (V status/phrase); 404 `ErrorItemNotFound`; 409; 412; 410 (V statuses).
- [ ] Error body: `{"error":{"code","message","innerError":{"date","request-id","client-request-id"}}}` (V schema); randomise the casing of `innerError`/`innererror` (V discrepancy).
- [ ] Reply/forward (later): `POST .../reply`, `.../forward` -> **202** no body; `comment`+`message.body` both set -> 400; forward without recipients or with both -> 400 (V); requires `Mail.Send` scope in the access token else 403 (U); copy appears in Sent Items (V).
- [ ] Scope enforcement: tokens with `Mail.ReadWrite` can PATCH/move; tokens lacking `Mail.ReadWrite` get 403 on PATCH/move (U).

### 11.9 Meta-tests every contract suite should run

1. Client sends `Prefer: IdType="ImmutableId"` on **every** Graph request (assert on the recorded request log).
2. Client never re-adds `$select`/`$filter` to a `nextLink`/`deltaLink` and never mutates the tokens.
3. Client never calls `/attachments/{id}/$value`, `/messages/{id}/$value`, or reads `contentBytes`.
4. Client never exceeds 3-4 in-flight requests; honours `Retry-After`; stops hammering during 429.
5. After 410/`syncStateNotFound` the client resyncs and **removes locally any message not returned in the full resync**.
6. After move-out-then-back-in with the "immutable gap" mode, the client's periodic reconcile restores the message.
7. Client handles `isRead` as boolean or string, `@odata.type` with or without `#`, `innerError`/`innererror`, and emits no secrets/tokens in logs.
8. Token refresh stores the new refresh token before discarding the old one; `AADSTS530036` forces re-auth.

---

## 12. Open questions to confirm against a real tenant (all UNVERIFIED above)

1. Does `GET /me/mailFolders/archive` return 404 on a mailbox that has never used Archive, and what is the exact error code?
2. Is `/me/mailFolders/inbox/messages/delta` (well-known name in the delta path) accepted, and what folder form do the returned links use?
3. Exact status and `error.code` for an expired/evicted messages deltaLink (400 vs 404 vs 410; `syncStateNotFound` vs `ErrorSyncStateNotFound`).
4. Does `flag` appear in delta `$select` and does a flag change produce a delta entry? Does `$select` work on PATCH?
5. Behaviour of an old (mutable) id after `move` when `Prefer: IdType="ImmutableId"` is sent only on the move but not on the delta.
6. Are multiple `Prefer` values accepted comma-joined? (one header line `Prefer: IdType="ImmutableId", outlook.body-content-type="text"`)
7. Is `slow_down` ever returned by `login.microsoftonline.com` for device code?
8. Is `$select` honoured on `/attachments`, and does `contentBytes` still appear when `$select` omits it?
9. Does the `complete` flag require `completedDateTime`, and does `notFlagged` wipe start/due?
10. Whether `Mail.ReadWrite` and `offline_access` need admin consent in the target tenant, and whether CA blocks device code there (test with a non-admin user and with report-only logs).
