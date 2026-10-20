#include "slowmail/graph/http_transport.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <mutex>

namespace slowmail::graph {

namespace {

constexpr std::size_t kMaxResponseBytes = 8u * 1024u * 1024u;  // guard against hostile/oversized responses

void ensure_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Collector {
    std::string* body = nullptr;
    std::map<std::string, std::string>* headers = nullptr;
    bool overflow = false;
};

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* c = static_cast<Collector*>(userdata);
    const std::size_t n = size * nmemb;
    if (c->body->size() + n > kMaxResponseBytes) {
        c->overflow = true;
        return 0;  // abort transfer
    }
    c->body->append(ptr, n);
    return n;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::size_t header_cb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* c = static_cast<Collector*>(userdata);
    const std::size_t n = size * nmemb;
    const std::string line(ptr, n);
    if (line.compare(0, 5, "HTTP/") == 0) {
        c->headers->clear();  // new response (redirect hop, 100-continue, proxy CONNECT)
        return n;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return n;
    std::string name = trim(line.substr(0, colon));
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    const std::string value = trim(line.substr(colon + 1));
    auto it = c->headers->find(name);
    if (it == c->headers->end()) {
        (*c->headers)[name] = value;
    } else {
        it->second += ", " + value;
    }
    return n;
}

bool has_crlf(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

// Plain http is tolerated only for loopback (the local fake Graph/Entra server). Everything else is https only.
bool is_loopback_http(const std::string& u) {
    static const char* const kPrefixes[] = {"http://127.0.0.1", "http://localhost", "http://[::1]"};
    for (const char* p : kPrefixes) {
        const std::string pre(p);
        if (u.compare(0, pre.size(), pre) == 0) {
            if (u.size() == pre.size()) return true;
            const char next = u[pre.size()];
            if (next == ':' || next == '/' || next == '?') return true;
        }
    }
    return false;
}

core::Error make_error(core::ErrorKind kind, const std::string& msg) {
    core::Error e;
    e.kind = kind;
    e.message = msg;
    return e;
}

struct SlistGuard {
    curl_slist* list = nullptr;
    ~SlistGuard() { curl_slist_free_all(list); }
};

struct HandleGuard {
    CURL* h = nullptr;
    ~HandleGuard() {
        if (h) curl_easy_cleanup(h);
    }
};

} // namespace

CurlTransport::CurlTransport() { ensure_global_init(); }
CurlTransport::~CurlTransport() = default;

core::Outcome<HttpResponse> CurlTransport::send(const HttpRequest& req) {
    try {
        ensure_global_init();

        static const char* const kMethods[] = {"GET", "POST", "PATCH", "PUT", "DELETE"};
        bool method_ok = false;
        for (const char* m : kMethods) method_ok = method_ok || req.method == m;
        if (!method_ok) return make_error(core::ErrorKind::Invalid, "unsupported HTTP method");
        if (req.url.empty() || has_crlf(req.url)) return make_error(core::ErrorKind::Invalid, "invalid request URL");

        HandleGuard handle;
        handle.h = curl_easy_init();
        if (!handle.h) return make_error(core::ErrorKind::Network, "curl_easy_init failed");
        CURL* h = handle.h;

        SlistGuard hdrs;
        for (const auto& kv : req.headers) {
            if (kv.first.empty() || has_crlf(kv.first) || has_crlf(kv.second) ||
                kv.first.find(':') != std::string::npos) {
                return make_error(core::ErrorKind::Invalid, "invalid request header");
            }
            const std::string line = kv.first + ": " + kv.second;
            curl_slist* next = curl_slist_append(hdrs.list, line.c_str());
            if (!next) return make_error(core::ErrorKind::Network, "out of memory building headers");
            hdrs.list = next;
        }
        {
            curl_slist* next = curl_slist_append(hdrs.list, "Expect:");  // never wait for 100-continue
            if (next) hdrs.list = next;
        }

        std::string resp_body;
        HttpResponse resp;
        Collector collector;
        collector.body = &resp_body;
        collector.headers = &resp.headers;

        curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        // TLS verification is always on and cannot be switched off through this class.
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(h, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));

        std::string ca = ca_bundle_;
        if (ca.empty()) {
            if (const char* e = std::getenv("CURL_CA_BUNDLE"); e && *e) ca = e;
        }
        if (ca.empty()) {
            if (const char* e = std::getenv("SSL_CERT_FILE"); e && *e) ca = e;
        }
        if (!ca.empty()) curl_easy_setopt(h, CURLOPT_CAINFO, ca.c_str());

        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, is_loopback_http(req.url) ? "http,https" : "https");
#else
        curl_easy_setopt(h, CURLOPT_PROTOCOLS,
                         is_loopback_http(req.url) ? (CURLPROTO_HTTP | CURLPROTO_HTTPS) : CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
        const long timeout = req.timeout_seconds > 0 ? req.timeout_seconds : 30;
        curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, timeout < 15 ? timeout : 15L);
        curl_easy_setopt(h, CURLOPT_USERAGENT, user_agent_.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs.list);
        curl_easy_setopt(h, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);

        if (req.method == "GET") {
            curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
        } else {
            if (req.method == "POST") {
                curl_easy_setopt(h, CURLOPT_POST, 1L);
            } else {
                curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, req.method.c_str());
            }
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, req.body.c_str());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(req.body.size()));
        }

        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &collector);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, header_cb);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &collector);

        const CURLcode rc = curl_easy_perform(h);
        if (rc != CURLE_OK) {
            if (collector.overflow) return make_error(core::ErrorKind::Network, "response too large");
            // Message intentionally carries no URL and no query string.
            const core::ErrorKind kind = (rc == CURLE_UNSUPPORTED_PROTOCOL || rc == CURLE_URL_MALFORMAT)
                                             ? core::ErrorKind::Invalid
                                             : core::ErrorKind::Network;
            return make_error(kind, std::string("HTTP transport error: ") + curl_easy_strerror(rc) + " (curl " +
                                        std::to_string(static_cast<int>(rc)) + ")");
        }
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        resp.status = static_cast<int>(status);
        resp.body = std::move(resp_body);
        return resp;
    } catch (const std::exception&) {
        return make_error(core::ErrorKind::Network, "HTTP transport internal error");
    } catch (...) {
        return make_error(core::ErrorKind::Network, "HTTP transport internal error");
    }
}

} // namespace slowmail::graph
