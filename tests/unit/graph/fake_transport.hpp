// Scripted HttpTransport for Graph/auth unit tests. Responses are consumed in queue order, each matched by
// method (empty = any) and URL substring. Unmatched requests fail with a Network error and are counted.
#pragma once
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "slowmail/graph/http_transport.hpp"

namespace slowmail::graph::testing {

class FakeTransport final : public HttpTransport {
public:
    struct Scripted {
        std::string method;
        std::string url_contains;
        std::optional<HttpResponse> response;
        std::optional<core::Error> error;
    };

    void expect(const std::string& method, const std::string& url_contains, int status, const std::string& body,
                std::map<std::string, std::string> headers = {}) {
        HttpResponse r;
        r.status = status;
        r.body = body;
        r.headers = std::move(headers);
        queue_.push_back({method, url_contains, r, std::nullopt});
    }

    void expect_error(const std::string& method, const std::string& url_contains, core::Error e) {
        queue_.push_back({method, url_contains, std::nullopt, std::move(e)});
    }

    core::Outcome<HttpResponse> send(const HttpRequest& req) override {
        requests_.push_back(req);
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            const bool method_ok = it->method.empty() || it->method == req.method;
            const bool url_ok = it->url_contains.empty() || req.url.find(it->url_contains) != std::string::npos;
            if (method_ok && url_ok) {
                Scripted s = std::move(*it);
                queue_.erase(it);
                if (s.error) return *s.error;
                return *s.response;
            }
        }
        ++unmatched_;
        core::Error e;
        e.kind = core::ErrorKind::Network;
        e.message = "FakeTransport: no scripted response for " + req.method + " " + req.url;
        return e;
    }

    const std::vector<HttpRequest>& requests() const { return requests_; }
    std::size_t unmatched() const { return unmatched_; }
    std::size_t pending() const { return queue_.size(); }

private:
    std::deque<Scripted> queue_;
    std::vector<HttpRequest> requests_;
    std::size_t unmatched_ = 0;
};

} // namespace slowmail::graph::testing
