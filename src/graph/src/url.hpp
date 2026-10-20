// Internal URL / form helpers for the Graph adapter. Header-only, pure, no I/O.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace slowmail::graph::url {

inline bool is_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == '.' || c == '~';
}

// RFC 3986 percent-encoding. Unreserved characters and any byte listed in `keep` are left as is.
inline std::string percent_encode(const std::string& s, const char* keep = "") {
    static const char* const kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + s.size() / 2);
    for (char c : s) {
        bool kept = is_unreserved(c);
        for (const char* k = keep; !kept && *k; ++k) kept = (*k == c);
        if (kept) {
            out.push_back(c);
        } else {
            const auto u = static_cast<unsigned char>(c);
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 0x0F]);
        }
    }
    return out;
}

// Encodes one URL path segment (message ids may contain '/', '+', '=').
inline std::string encode_path_segment(const std::string& s) { return percent_encode(s); }

// application/x-www-form-urlencoded: space -> '+', everything but unreserved percent-encoded.
inline std::string form_encode_component(const std::string& s) {
    std::string enc = percent_encode(s);
    std::string out;
    out.reserve(enc.size());
    for (std::size_t i = 0; i < enc.size(); ++i) {
        if (enc[i] == '%' && enc.compare(i, 3, "%20") == 0) {
            out.push_back('+');
            i += 2;
        } else {
            out.push_back(enc[i]);
        }
    }
    return out;
}

inline std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& f : fields) {
        if (!out.empty()) out.push_back('&');
        out += form_encode_component(f.first);
        out.push_back('=');
        out += form_encode_component(f.second);
    }
    return out;
}

// Appends "key=value" to a URL, using '?' or '&'. OData option names ("$select") keep their '$' and
// list values keep ',' so URLs stay readable; everything else is percent-encoded.
inline std::string append_query(const std::string& base, const std::string& key, const std::string& value) {
    std::string out = base;
    out.push_back(base.find('?') == std::string::npos ? '?' : '&');
    out += percent_encode(key, "$");
    out.push_back('=');
    out += percent_encode(value, ",");
    return out;
}

// "https://host:port" part of a URL (scheme and authority, lower-cased), empty if not an absolute URL.
inline std::string origin_of(const std::string& u) {
    const auto scheme_end = u.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0) return {};
    const auto auth_end = u.find_first_of("/?#", scheme_end + 3);
    std::string o = u.substr(0, auth_end);
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return o;
}

} // namespace slowmail::graph::url
