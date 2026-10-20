#pragma once
#include <map>
#include <string>

#include "slowmail/core/types.hpp"

namespace slowmail::graph {

struct HttpRequest {
    std::string method;                         // GET, POST, PATCH
    std::string url;
    std::map<std::string, std::string> headers; // case-sensitive keys as written
    std::string body;                           // empty for GET
    int timeout_seconds = 30;
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers; // keys lower-cased
    std::string body;
};

// Minimal HTTP transport so the Graph client is testable without the network.
class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Transport-level failures (DNS, TLS, timeout) -> ErrorKind::Network. HTTP error statuses are NOT errors here.
    virtual core::Outcome<HttpResponse> send(const HttpRequest& req) = 0;
};

// libcurl implementation. TLS verification is always on; CA bundle path can be set (e.g. CURL_CA_BUNDLE / SSL_CERT_FILE)
// but verification can NOT be disabled. Honours HTTPS_PROXY from the environment via libcurl defaults.
class CurlTransport final : public HttpTransport {
public:
    CurlTransport();
    ~CurlTransport() override;
    core::Outcome<HttpResponse> send(const HttpRequest& req) override;
    void set_ca_bundle(const std::string& path) { ca_bundle_ = path; }
    void set_user_agent(const std::string& ua) { user_agent_ = ua; }
private:
    std::string ca_bundle_;
    std::string user_agent_ = "SlowMail/0.1 (reMarkable Paper Pro)";
};

} // namespace slowmail::graph
