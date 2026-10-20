#include "slowmail/graph/graph_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>

#include <nlohmann/json.hpp>

#include "slowmail/core/log.hpp"
#include "slowmail/core/sanitize.hpp"
#include "url.hpp"

namespace slowmail::graph {

using nlohmann::json;

namespace {

constexpr const char* kMessageSelect =
    "id,conversationId,subject,from,receivedDateTime,isRead,flag,bodyPreview,hasAttachments,changeKey,parentFolderId";
constexpr const char* kBodySelect = "id,body,bodyPreview,hasAttachments";
constexpr const char* kAttachmentSelect = "id,name,contentType,size,isInline";  // never contentBytes
constexpr const char* kAccountSelect = "id,userPrincipalName,mail";
constexpr std::size_t kMaxIdLength = 1024;

core::Error make_error(core::ErrorKind kind, std::string msg, int status = 0, std::string code = {}) {
    core::Error e;
    e.kind = kind;
    e.message = core::redact(msg);
    e.http_status = status;
    e.code = std::move(code);
    return e;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string str_of(const json& j, const char* key) {
    if (!j.is_object()) return {};
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// Graph samples show booleans as JSON booleans, but the docs contain string typos ("true"); accept both.
bool bool_of(const json& j, const char* key, bool fallback) {
    if (!j.is_object()) return fallback;
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) {
        const std::string v = lower(it->get<std::string>());
        if (v == "true") return true;
        if (v == "false") return false;
    }
    return fallback;
}

std::string clean_code(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (url::is_unreserved(c)) out.push_back(c);
        if (out.size() >= 64) break;
    }
    return out;
}

// Case-insensitive lookup ("innerError" vs "innererror" both occur in Graph docs).
const json* find_ci(const json& obj, const char* key) {
    if (!obj.is_object()) return nullptr;
    const std::string k = lower(key);
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (lower(it.key()) == k) return &it.value();
    }
    return nullptr;
}

std::optional<int> parse_retry_after(const HttpResponse& r) {
    auto it = r.headers.find("retry-after");
    if (it == r.headers.end()) return std::nullopt;
    const std::string& v = it->second;
    if (v.empty() || v.size() > 9) return std::nullopt;
    for (char c : v) {
        if (c < '0' || c > '9') return std::nullopt;  // HTTP-date form is not used by Graph; treat as absent
    }
    return std::stoi(v);
}

std::string new_request_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* const kHex = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) id.push_back('-');
        id.push_back(kHex[rng() & 0xF]);
    }
    return id;
}

std::string with_select(const std::string& base, const char* select) { return url::append_query(base, "$select", select); }

struct ParsedDelta {
    core::DeltaPage page;
    std::vector<std::size_t> partial;  // indices into page.changed that lacked subject/from/receivedDateTime
};

core::Outcome<ParsedDelta> parse_delta_impl(const std::string& body) {
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return make_error(core::ErrorKind::Malformed, "delta response is not a JSON object");
    }
    ParsedDelta out;
    const std::string next = str_of(j, "@odata.nextLink");
    const std::string delta = str_of(j, "@odata.deltaLink");
    if (!next.empty()) out.page.next_link = next;
    if (!delta.empty()) out.page.delta_link = delta;

    auto it = j.find("value");
    if (it == j.end()) {
        if (!out.page.next_link && !out.page.delta_link) {
            return make_error(core::ErrorKind::Malformed, "delta response has neither value nor links");
        }
        return out;
    }
    if (!it->is_array()) return make_error(core::ErrorKind::Malformed, "delta value is not an array");

    std::size_t skipped = 0;
    for (const json& e : *it) {
        if (!e.is_object()) {
            ++skipped;
            continue;
        }
        if (e.contains("@removed")) {
            const std::string id = str_of(e, "id");
            if (id.empty() || id.size() > kMaxIdLength) {
                ++skipped;
            } else {
                out.page.removed_ids.push_back(id);
            }
            continue;
        }
        auto m = GraphBackend::parse_message_summary(e);
        if (!m.ok()) {
            ++skipped;
            continue;
        }
        const bool partial = !e.contains("subject") || !e.contains("from") || !e.contains("receivedDateTime");
        if (partial) out.partial.push_back(out.page.changed.size());
        out.page.changed.push_back(std::move(m.value()));
    }
    if (skipped > 0) {
        SLOWMAIL_LOG_WARN("delta page: skipped " + std::to_string(skipped) + " unusable entries");
    }
    return out;
}

core::AttachmentKind classify_attachment(const std::string& odata_type) {
    std::string t = lower(odata_type);
    if (!t.empty() && t[0] == '#') t.erase(0, 1);  // the docs sample omits the '#'; the service sends it
    const std::string prefix = "microsoft.graph.";
    if (t.compare(0, prefix.size(), prefix) == 0) t.erase(0, prefix.size());
    if (t == "fileattachment") return core::AttachmentKind::File;
    if (t == "itemattachment") return core::AttachmentKind::Item;
    if (t == "referenceattachment") return core::AttachmentKind::Reference;
    return core::AttachmentKind::Unknown;
}

std::vector<core::AttachmentMeta> parse_attachments(const std::string& body) {
    std::vector<core::AttachmentMeta> out;
    const json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return out;
    auto it = j.find("value");
    if (it == j.end() || !it->is_array()) return out;
    for (const json& a : *it) {
        if (!a.is_object()) continue;
        if (out.size() >= core::Limits::kMaxAttachmentsListed) break;
        core::AttachmentMeta m;
        m.id = str_of(a, "id");  // contentBytes is deliberately never read
        m.name = str_of(a, "name");
        m.content_type = str_of(a, "contentType");  // null for item/reference attachments
        m.size = -1;
        auto s = a.find("size");
        if (s != a.end() && s->is_number()) {
            const double v = s->get<double>();
            m.size = v >= 0 ? static_cast<std::int64_t>(v) : -1;
        }
        m.is_inline = bool_of(a, "isInline", false);
        m.kind = classify_attachment(str_of(a, "@odata.type"));
        out.push_back(std::move(m));
    }
    return core::sanitize_attachments(out);
}

} // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------
core::Error GraphBackend::map_http_error(const HttpResponse& r) {
    core::Error e;
    e.http_status = r.status;

    std::string request_id;
    {
        const json j = json::parse(r.body, nullptr, false);  // 503 from a front proxy may be HTML
        if (!j.is_discarded()) {
            if (const json* err = find_ci(j, "error"); err && err->is_object()) {
                e.code = clean_code(str_of(*err, "code"));
                if (const json* inner = find_ci(*err, "innererror"); inner && inner->is_object()) {
                    if (e.code.empty()) e.code = clean_code(str_of(*inner, "code"));
                    request_id = clean_code(str_of(*inner, "request-id"));
                }
            }
        }
    }
    const auto retry_after = parse_retry_after(r);
    const int s = r.status;
    if (s == 401) {
        e.kind = core::ErrorKind::Auth;
    } else if (s == 403) {
        e.kind = core::ErrorKind::Forbidden;
    } else if (s == 404) {
        e.kind = core::ErrorKind::NotFound;
    } else if (s == 409 || s == 412) {
        e.kind = core::ErrorKind::Conflict;
    } else if (s == 410) {
        e.kind = core::ErrorKind::Gone;
    } else if (s == 429 || (s == 503 && retry_after)) {
        e.kind = core::ErrorKind::Throttled;
        e.retry_after_seconds = retry_after;
    } else if (s >= 500 && s <= 599) {
        e.kind = core::ErrorKind::Server;
    } else if (s >= 300 && s < 400) {
        e.kind = core::ErrorKind::Malformed;  // redirects are never followed
    } else {
        e.kind = core::ErrorKind::Invalid;
    }
    e.message = "Graph request failed: HTTP " + std::to_string(s);
    if (!e.code.empty()) e.message += " " + e.code;
    if (!request_id.empty()) e.message += " (request-id " + request_id + ")";
    return e;
}

core::Outcome<core::MessageSummary> GraphBackend::parse_message_summary(const json& j) {
    if (!j.is_object()) return make_error(core::ErrorKind::Malformed, "message entry is not an object");
    core::MessageSummary m;
    m.id = str_of(j, "id");
    if (m.id.empty() || m.id.size() > kMaxIdLength) {
        return make_error(core::ErrorKind::Malformed, "message entry has no usable id");
    }
    m.conversation_id = str_of(j, "conversationId");
    if (m.conversation_id.size() > kMaxIdLength) m.conversation_id.clear();
    m.subject = core::sanitize_line(str_of(j, "subject"), core::Limits::kMaxSubject);

    const json* from = nullptr;
    if (auto it = j.find("from"); it != j.end() && it->is_object()) from = &*it;
    if (!from) {
        if (auto it = j.find("sender"); it != j.end() && it->is_object()) from = &*it;
    }
    if (from) {
        if (auto ea = from->find("emailAddress"); ea != from->end() && ea->is_object()) {
            m.from.name = core::sanitize_line(str_of(*ea, "name"), core::Limits::kMaxSubject);
            m.from.address = core::sanitize_line(str_of(*ea, "address"), core::Limits::kMaxAddress);
        }
    }
    m.received_at_iso = core::sanitize_line(str_of(j, "receivedDateTime"), 64);
    std::int64_t epoch = 0;
    if (!m.received_at_iso.empty() && core::parse_iso8601_utc(m.received_at_iso, epoch)) m.received_at_epoch = epoch;
    m.is_read = bool_of(j, "isRead", false);
    if (auto it = j.find("flag"); it != j.end() && it->is_object()) {
        m.flag_status = core::flag_status_from_string(str_of(*it, "flagStatus"));
    }
    m.body_preview = core::sanitize_line(str_of(j, "bodyPreview"), core::Limits::kMaxPreview);
    m.has_attachments = bool_of(j, "hasAttachments", false);
    m.change_key = core::sanitize_line(str_of(j, "changeKey"), 256);
    m.parent_folder_id = core::sanitize_line(str_of(j, "parentFolderId"), 512);
    return m;
}

core::Outcome<core::DeltaPage> GraphBackend::parse_delta_page(const std::string& body) {
    try {
        auto p = parse_delta_impl(body);
        if (!p.ok()) return p.error();
        return std::move(p.value().page);
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "delta response could not be processed");
    }
}

// ---------------------------------------------------------------------------
// Construction and transport wrapper
// ---------------------------------------------------------------------------
GraphBackend::GraphBackend(GraphConfig config, std::shared_ptr<HttpTransport> transport,
                           std::shared_ptr<TokenProvider> tokens, std::shared_ptr<core::Clock> clock)
    : config_(std::move(config)),
      transport_(std::move(transport)),
      tokens_(std::move(tokens)),
      clock_(std::move(clock)) {
    while (!config_.base_url.empty() && config_.base_url.back() == '/') config_.base_url.pop_back();
}

core::Outcome<HttpResponse> GraphBackend::call(const std::string& method, const std::string& url,
                                               const std::string& body,
                                               const std::map<std::string, std::string>& extra_headers) {
    try {
        HttpRequest req;
        req.method = method;
        req.url = url;
        req.body = body;
        req.headers["Accept"] = "application/json";
        req.headers["client-request-id"] = new_request_id();
        if (!body.empty()) req.headers["Content-Type"] = "application/json";

        // Prefer values are merged into one header; IdType=ImmutableId goes on EVERY request (header is per-request).
        std::string prefer;
        for (const auto& kv : extra_headers) {
            if (lower(kv.first) == "prefer") {
                if (!kv.second.empty()) prefer = kv.second;
            } else {
                req.headers[kv.first] = kv.second;
            }
        }
        if (config_.prefer_immutable_ids) {
            if (!prefer.empty()) prefer += ", ";
            prefer += "IdType=\"ImmutableId\"";
        }
        if (!prefer.empty()) req.headers["Prefer"] = prefer;

        bool auth_retried = false;
        int throttle_retries = 0;
        for (;;) {
            auto tok = tokens_->access_token();
            if (!tok.ok()) return tok.error();
            req.headers["Authorization"] = "Bearer " + tok.value();

            auto resp = transport_->send(req);
            if (!resp.ok()) return resp.error();
            const HttpResponse& r = resp.value();
            if (r.status >= 200 && r.status < 300) return r;

            if (r.status == 401 && !auth_retried) {
                auth_retried = true;
                tokens_->invalidate();
                continue;
            }
            if ((r.status == 429 || r.status == 503) && throttle_retries < config_.max_retries_on_throttle) {
                const auto ra = parse_retry_after(r);
                int wait = 0;
                if (ra) {
                    if (*ra > config_.max_retry_after_wait_seconds) return map_http_error(r);
                    wait = std::max(1, *ra);
                } else {
                    // No Retry-After: exponential backoff 1, 2, 4, ... capped.
                    const int shift = std::min(throttle_retries, 20);
                    wait = std::min(1 << shift, std::max(1, config_.max_retry_after_wait_seconds));
                }
                ++throttle_retries;
                SLOWMAIL_LOG_INFO("Graph throttled (HTTP " + std::to_string(r.status) + "); retry " +
                                  std::to_string(throttle_retries) + " in " + std::to_string(wait) + "s");
                if (config_.sleep_on_retry) std::this_thread::sleep_for(std::chrono::seconds(wait));
                continue;
            }
            return map_http_error(r);
        }
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Network, "Graph call failed unexpectedly");
    }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
core::Outcome<core::DeltaPage> GraphBackend::fetch_inbox_delta(const std::string& link) {
    try {
        std::string url;
        if (link.empty()) {
            url = with_select(config_.base_url + "/me/mailFolders/inbox/messages/delta", kMessageSelect);
        } else {
            // Links are opaque and used verbatim, but never sent (with a bearer token) to a foreign origin.
            if (url::origin_of(link) != url::origin_of(config_.base_url)) {
                return make_error(core::ErrorKind::Invalid, "delta link does not belong to the Graph endpoint");
            }
            url = link;
        }
        const std::map<std::string, std::string> hdrs = {
            {"Prefer", "odata.maxpagesize=" + std::to_string(config_.page_size)}};
        auto r = call("GET", url, "", hdrs);
        if (!r.ok()) {
            core::Error e = r.error();
            const int s = e.http_status;
            const bool sync_state = lower(e.code).find("syncstate") != std::string::npos;
            const bool other_4xx = s >= 400 && s < 500 && s != 401 && s != 403 && s != 404 && s != 429;
            if (!link.empty() && e.kind != core::ErrorKind::Gone && other_4xx && sync_state) {
                e.kind = core::ErrorKind::Gone;  // expired / invalid delta token: full resync required
            }
            return e;
        }
        auto parsed = parse_delta_impl(r.value().body);
        if (!parsed.ok()) return parsed.error();
        core::DeltaPage page = std::move(parsed.value().page);

        // Updated items may carry only id + changed fields. Core cannot tell, so fetch the full item.
        if (!parsed.value().partial.empty()) {
            std::vector<bool> drop(page.changed.size(), false);
            for (std::size_t idx : parsed.value().partial) {
                const std::string id = page.changed[idx].id;
                const std::string murl =
                    with_select(config_.base_url + "/me/messages/" + url::encode_path_segment(id), kMessageSelect);
                auto g = call("GET", murl, "", {});
                if (!g.ok()) {
                    if (g.error().kind == core::ErrorKind::NotFound && g.error().code != "ErrorFolderNotFound") {
                        page.removed_ids.push_back(id);
                        drop[idx] = true;
                        continue;
                    }
                    return g.error();
                }
                const json mj = json::parse(g.value().body, nullptr, false);
                if (mj.is_discarded()) return make_error(core::ErrorKind::Malformed, "message response is not JSON");
                auto full = parse_message_summary(mj);
                if (!full.ok()) return full.error();
                page.changed[idx] = std::move(full.value());
            }
            std::vector<core::MessageSummary> kept;
            kept.reserve(page.changed.size());
            for (std::size_t i = 0; i < page.changed.size(); ++i) {
                if (!drop[i]) kept.push_back(std::move(page.changed[i]));
            }
            page.changed = std::move(kept);
        }
        return page;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "delta response could not be processed");
    }
}

core::Outcome<core::MessageBody> GraphBackend::fetch_body(const std::string& message_id) {
    try {
        if (message_id.empty()) return make_error(core::ErrorKind::Invalid, "empty message id");
        const std::string enc = url::encode_path_segment(message_id);
        auto r = call("GET", with_select(config_.base_url + "/me/messages/" + enc, kBodySelect), "",
                      {{"Prefer", "outlook.body-content-type=\"text\""}});
        if (!r.ok()) return r.error();
        const json j = json::parse(r.value().body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return make_error(core::ErrorKind::Malformed, "message response is not JSON");
        }
        auto bit = j.find("body");
        if (bit == j.end() || !bit->is_object() || !bit->contains("content") || !(*bit)["content"].is_string()) {
            return make_error(core::ErrorKind::Malformed, "message response has no body");
        }
        core::MessageBody out;
        out.id = message_id;
        const std::string content = (*bit)["content"].get<std::string>();
        bool truncated = false;
        if (lower(str_of(*bit, "contentType")) == "html") {  // contentType is authoritative, not the Prefer header
            out.was_html = true;
            out.text = core::html_to_text(content, core::Limits::kMaxBodyChars, truncated);
        } else {
            out.text = core::sanitize_text(content, core::Limits::kMaxBodyChars, truncated);
        }
        out.truncated = truncated;

        // hasAttachments is false for inline-only messages, so also look for cid: markers; missing field => ask.
        const bool want_list = bool_of(j, "hasAttachments", true) || content.find("[cid:") != std::string::npos;
        if (want_list) {
            auto a = call("GET", with_select(config_.base_url + "/me/messages/" + enc + "/attachments", kAttachmentSelect),
                          "", {});
            if (a.ok()) {
                out.attachments = parse_attachments(a.value().body);
            } else if (a.error().kind == core::ErrorKind::Forbidden || a.error().kind == core::ErrorKind::NotFound) {
                SLOWMAIL_LOG_WARN("attachment list unavailable (HTTP " + std::to_string(a.error().http_status) + ")");
            } else {
                return a.error();
            }
        }
        return out;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "message response could not be processed");
    }
}

core::Outcome<core::MoveResult> GraphBackend::move(const std::string& message_id, core::WellKnownFolder destination) {
    try {
        if (message_id.empty()) return make_error(core::ErrorKind::Invalid, "empty message id");
        const json body = {{"destinationId", core::to_string(destination)}};
        auto r = call("POST", config_.base_url + "/me/messages/" + url::encode_path_segment(message_id) + "/move",
                      body.dump(), {});
        if (!r.ok()) return r.error();  // Graph answers 201 Created; call() accepts any 2xx
        const json j = json::parse(r.value().body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return make_error(core::ErrorKind::Malformed, "move response is not JSON", r.value().status);
        }
        core::MoveResult m;
        m.new_id = str_of(j, "id");
        m.new_folder_id = core::sanitize_line(str_of(j, "parentFolderId"), 512);
        if (m.new_id.empty() || m.new_id.size() > kMaxIdLength) {
            return make_error(core::ErrorKind::Malformed, "move response has no id", r.value().status);
        }
        return m;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "move response could not be processed");
    }
}

core::Status GraphBackend::set_flag(const std::string& message_id, core::FlagStatus status) {
    try {
        if (message_id.empty()) return make_error(core::ErrorKind::Invalid, "empty message id");
        json flag = {{"flagStatus", core::to_string(status)}};
        if (status == core::FlagStatus::Complete) {
            std::string iso = core::format_iso8601_utc(clock_->now_wall());
            if (!iso.empty() && iso.back() == 'Z') iso.pop_back();
            flag["completedDateTime"] = {{"dateTime", iso + ".0000000"}, {"timeZone", "UTC"}};
        }
        const json body = {{"flag", flag}};
        auto r = call("PATCH", config_.base_url + "/me/messages/" + url::encode_path_segment(message_id), body.dump(), {});
        if (!r.ok()) return r.error();
        return core::Status::success();  // response is the full message; intentionally ignored
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "flag request could not be processed");
    }
}

core::Outcome<core::AccountInfo> GraphBackend::account() {
    try {
        auto r = call("GET", with_select(config_.base_url + "/me", kAccountSelect), "", {});
        if (!r.ok()) {
            if (r.error().kind == core::ErrorKind::Auth) {
                core::AccountInfo none;
                none.signed_in = false;
                return none;
            }
            return r.error();
        }
        const json j = json::parse(r.value().body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return make_error(core::ErrorKind::Malformed, "account response is not JSON");
        core::AccountInfo a;
        a.user_id = core::sanitize_line(str_of(j, "id"), 128);
        std::string addr = str_of(j, "mail");
        if (addr.empty()) addr = str_of(j, "userPrincipalName");
        a.display_address = core::sanitize_line(addr, core::Limits::kMaxAddress);
        a.signed_in = true;
        return a;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Malformed, "account response could not be processed");
    }
}

} // namespace slowmail::graph
