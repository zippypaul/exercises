// Sanitisation of untrusted mail content. Pure, bounded, no regex, no external libraries.
//
// Conventions:
//  * "max_chars" / "max_bytes" limits are measured in UTF-8 BYTES of the output and are always
//    applied on a code point boundary, so output is always valid UTF-8.
//  * Invalid UTF-8 input bytes are replaced by U+FFFD (one per bad byte).
//  * Memory is bounded by min(input size, limit) plus small constant-size scratch buffers.
#include "slowmail/core/sanitize.hpp"

#include <cstdio>
#include <string>

namespace slowmail::core {

namespace {

constexpr std::uint32_t kReplacement = 0xFFFD;
constexpr std::size_t kMaxHrefRaw = 4096;   // longest raw href attribute we are willing to look at
constexpr std::size_t kMaxUrlOut = 2048;    // longest URL we will print
constexpr std::size_t kMaxTagName = 24;
constexpr int kMaxListDepth = 16;

// ---------------------------------------------------------------------------
// UTF-8 helpers
// ---------------------------------------------------------------------------
std::uint32_t utf8_next(const std::string& s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int len = 0;
    std::uint32_t cp = 0;
    std::uint32_t min_cp = 0;
    if ((c & 0xE0) == 0xC0) {
        len = 2; cp = c & 0x1Fu; min_cp = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3; cp = c & 0x0Fu; min_cp = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4; cp = c & 0x07u; min_cp = 0x10000;
    } else {
        ++i;
        return kReplacement;
    }
    if (i + static_cast<std::size_t>(len) > s.size()) {
        ++i;
        return kReplacement;
    }
    for (int k = 1; k < len; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((cc & 0xC0) != 0x80) {
            ++i;
            return kReplacement;
        }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return kReplacement;
    }
    i += static_cast<std::size_t>(len);
    return cp;
}

std::size_t utf8_len(std::uint32_t cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

void utf8_append(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Characters removed outright: C0 controls (except \n \t \r \f, handled as whitespace), DEL, C1 controls,
// soft hyphen, zero-width space, bidi controls/overrides (incl. U+061C), word joiner, Mongolian vowel separator,
// interlinear annotation characters, tag characters and BOM. ZWJ/ZWNJ are kept
// because emoji sequences and some scripts need them.
bool is_stripped(std::uint32_t cp) {
    if (cp < 0x20) return !(cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f');
    if (cp >= 0x7F && cp <= 0x9F) return true;
    if (cp == 0xAD || cp == 0x200B || cp == 0x200E || cp == 0x200F) return true;
    if (cp >= 0x202A && cp <= 0x202E) return true;
    if (cp >= 0x2060 && cp <= 0x2064) return true;
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    if (cp == 0x061C || cp == 0x180E) return true;      // Arabic letter mark, Mongolian vowel separator
    if (cp >= 0xFFF9 && cp <= 0xFFFB) return true;      // interlinear annotation anchor/separator/terminator
    if (cp >= 0xE0000 && cp <= 0xE007F) return true;    // tag characters (invisible ASCII smuggling)
    return cp == 0xFEFF;
}

bool is_line_break_cp(std::uint32_t cp) { return cp == 0x2028 || cp == 0x2029; }

bool is_space_cp(std::uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

// ---------------------------------------------------------------------------
// ASCII helpers
// ---------------------------------------------------------------------------
bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ci_match(const std::string& s, std::size_t pos, const std::string& lit) {
    if (pos + lit.size() > s.size()) return false;
    for (std::size_t k = 0; k < lit.size(); ++k) {
        if (lc(s[pos + k]) != lit[k]) return false;
    }
    return true;
}

bool starts_with_ci(const std::string& s, const char* lit) {
    for (std::size_t k = 0; lit[k]; ++k) {
        if (k >= s.size() || lc(s[k]) != lit[k]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entity decoding
// ---------------------------------------------------------------------------
struct NamedEntity {
    const char* name;
    std::uint32_t cp;
};

const NamedEntity kEntities[] = {
    {"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", 0xA0},   {"copy", 0xA9},    {"reg", 0xAE},     {"trade", 0x2122}, {"hellip", 0x2026},
    {"mdash", 0x2014}, {"ndash", 0x2013}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"bull", 0x2022},  {"middot", 0xB7},
    {"euro", 0x20AC}, {"pound", 0xA3},   {"yen", 0xA5},     {"cent", 0xA2},    {"sect", 0xA7},
    {"deg", 0xB0},    {"plusmn", 0xB1},  {"times", 0xD7},   {"divide", 0xF7},  {"laquo", 0xAB},
    {"raquo", 0xBB},  {"shy", 0xAD},     {"zwnj", 0x200C},  {"zwj", 0x200D},   {"lrm", 0x200E},
    {"rlm", 0x200F},  {"iexcl", 0xA1},   {"iquest", 0xBF},  {"para", 0xB6},    {"frac12", 0xBD},
    {"frac14", 0xBC}, {"frac34", 0xBE},  {"micro", 0xB5},   {"larr", 0x2190},  {"rarr", 0x2192},
    {"uarr", 0x2191}, {"darr", 0x2193},  {"ensp", 0x2002},  {"emsp", 0x2003},  {"thinsp", 0x2009},
    {"hearts", 0x2665}, {"check", 0x2713}, {"dagger", 0x2020}, {"permil", 0x2030}, {"prime", 0x2032},
    {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"agrave", 0xE0},
    {"aacute", 0xE1}, {"uuml", 0xFC},    {"ouml", 0xF6},    {"auml", 0xE4},    {"szlig", 0xDF},
    {"ntilde", 0xF1}, {"ccedil", 0xE7},  {"Eacute", 0xC9},  {"Uuml", 0xDC},    {"Ouml", 0xD6},
    {"Auml", 0xC4},
};

// Windows-1252 interpretation of numeric references 0x80..0x9F (HTML spec behaviour).
const std::uint32_t kCp1252[32] = {
    0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};

std::uint32_t map_numeric(std::uint32_t v) {
    if (v == 0) return 0;  // stripped later
    if (v >= 0x80 && v <= 0x9F) return kCp1252[v - 0x80];
    if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return kReplacement;
    return v;
}

// s[pos] == '&'. Decodes within [pos, limit). On success sets cp and `end` (one past the entity).
bool decode_entity(const std::string& s, std::size_t pos, std::size_t limit, std::size_t& end, std::uint32_t& cp) {
    std::size_t p = pos + 1;
    if (p >= limit) return false;
    if (s[p] == '#') {
        ++p;
        const bool hex = p < limit && (s[p] == 'x' || s[p] == 'X');
        if (hex) ++p;
        const std::size_t digits_start = p;
        std::uint32_t v = 0;
        while (p < limit) {
            const char c = s[p];
            std::uint32_t d;
            if (is_digit(c)) {
                d = static_cast<std::uint32_t>(c - '0');
            } else if (hex && c >= 'a' && c <= 'f') {
                d = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (hex && c >= 'A' && c <= 'F') {
                d = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                break;
            }
            v = v * (hex ? 16u : 10u) + d;
            if (v > 0x10FFFF) v = 0x110000;  // clamp; keeps arithmetic far from overflow
            ++p;
        }
        if (p == digits_start) return false;
        if (p < limit && s[p] == ';') ++p;
        cp = map_numeric(v);
        end = p;
        return true;
    }
    const std::size_t name_start = p;
    while (p < limit && p - name_start <= 10 && is_alnum(s[p])) ++p;
    if (p == name_start || p >= limit || s[p] != ';') return false;
    const std::size_t name_len = p - name_start;
    for (const auto& e : kEntities) {
        const char* n = e.name;
        std::size_t k = 0;
        while (k < name_len && n[k] && n[k] == s[name_start + k]) ++k;
        if (k == name_len && n[k] == '\0') {
            cp = e.cp;
            end = p + 1;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bounded text builder used by html_to_text
// ---------------------------------------------------------------------------
class TextOut {
public:
    TextOut(std::size_t cap, std::size_t input_size) : cap_(cap) {
        out_.reserve((input_size < cap ? input_size : cap) + 8);
    }

    bool truncated() const { return truncated_; }
    std::size_t size() const { return out_.size(); }
    const std::string& str() const { return out_; }

    // Text content from the document (already entity-decoded).
    void put_cp(std::uint32_t cp, bool pre) {
        if (is_stripped(cp)) return;
        if (is_space_cp(cp)) {
            if (pre) {
                if (cp == '\n' || is_line_break_cp(cp)) {
                    hard_newline();
                } else if (cp == '\t') {
                    literal("\t", 1);
                } else if (cp == ' ' || cp == 0xA0) {
                    literal(" ", 1);
                }
                // \r, \f and exotic spaces are dropped inside <pre>
            } else {
                space();
            }
            return;
        }
        if (full_) {
            truncated_ = true;
            return;
        }
        flush_space();
        if (full_) {
            truncated_ = true;
            return;
        }
        std::string tmp;
        utf8_append(tmp, cp);
        if (!append(tmp.data(), tmp.size())) {
            truncated_ = true;
            return;
        }
        nl_run_ = 0;
    }

    // Non-whitespace ASCII content (markers, URLs).
    void word(const std::string& w) {
        if (w.empty()) return;
        if (full_) {
            truncated_ = true;
            return;
        }
        flush_space();
        if (full_ || !append(w.data(), w.size())) {
            truncated_ = true;
            return;
        }
        nl_run_ = 0;
    }

    // Raw bytes with no pending-space handling (list indentation and markers).
    void literal(const char* s, std::size_t n) {
        if (out_.empty() && s[0] == ' ') return;
        if (full_) {
            if (s[0] != ' ' && s[0] != '\t') truncated_ = true;
            return;
        }
        if (!append(s, n)) {
            if (s[0] != ' ' && s[0] != '\t') truncated_ = true;
            return;
        }
        nl_run_ = 0;
    }

    void space() {
        if (!out_.empty()) pending_space_ = true;
    }

    void hard_newline() {
        pending_space_ = false;
        if (out_.empty() || nl_run_ >= 2) return;
        if (append("\n", 1)) ++nl_run_;
    }
    void ensure_newline() {
        if (out_.empty() || nl_run_ >= 1) {
            pending_space_ = false;
            return;
        }
        hard_newline();
    }
    void ensure_blank() {
        hard_newline_if_needed(2);
    }

    std::string finish() {
        std::size_t e = out_.size();
        while (e > 0 && (out_[e - 1] == ' ' || out_[e - 1] == '\n' || out_[e - 1] == '\t')) --e;
        out_.resize(e);
        return std::move(out_);
    }

private:
    void hard_newline_if_needed(int want) {
        pending_space_ = false;
        if (out_.empty()) return;
        while (nl_run_ < want) {
            const int before = nl_run_;
            hard_newline();
            if (nl_run_ == before) break;
        }
    }
    void flush_space() {
        if (pending_space_ && nl_run_ == 0 && !out_.empty() && out_.back() != ' ') append(" ", 1);
        pending_space_ = false;
    }
    bool append(const char* s, std::size_t n) {
        if (full_) return false;
        if (out_.size() + n > cap_) {
            full_ = true;
            return false;
        }
        out_.append(s, n);
        return true;
    }

    std::string out_;
    std::size_t cap_;
    bool truncated_ = false;
    bool full_ = false;
    bool pending_space_ = false;
    int nl_run_ = 0;
};

// ---------------------------------------------------------------------------
// HTML tokenizer / converter
// ---------------------------------------------------------------------------
struct Tag {
    std::string name;  // lower-case, truncated to kMaxTagName
    bool end = false;
    bool self_closing = false;
    bool terminated = false;  // found closing '>'
    bool has_href = false;
    std::string href;         // raw (entities not yet decoded), bounded
    std::size_t next = 0;     // index after the tag
};

bool is_raw_text_name(const std::string& n) {
    return n == "script" || n == "style" || n == "title" || n == "noembed" || n == "noframes";
}

bool is_drop_name(const std::string& n) {
    return n == "head" || n == "template" || n == "iframe" || n == "object" || n == "svg" || n == "noscript";
}

bool is_paragraph_name(const std::string& n) {
    return n == "p" || n == "h1" || n == "h2" || n == "h3" || n == "h4" || n == "h5" || n == "h6" ||
           n == "blockquote" || n == "table" || n == "dl" || n == "address" || n == "figure" || n == "form" ||
           n == "fieldset" || n == "details";
}

bool is_line_name(const std::string& n) {
    return n == "div" || n == "tr" || n == "section" || n == "article" || n == "header" || n == "footer" ||
           n == "nav" || n == "aside" || n == "main" || n == "dd" || n == "dt" || n == "caption" ||
           n == "thead" || n == "tbody" || n == "tfoot" || n == "center" || n == "figcaption" ||
           n == "summary" || n == "legend";
}

class HtmlConverter {
public:
    HtmlConverter(const std::string& html, std::size_t max_chars) : h_(html), n_(html.size()), out_(max_chars, html.size()) {}

    std::string run(bool& truncated) {
        std::size_t i = 0;
        while (i < n_ && !out_.truncated()) {
            const char ch = h_[i];
            if (ch == '<') {
                const std::size_t sp = skip_special(i);
                if (sp != std::string::npos) {
                    i = sp;
                    continue;
                }
                if (i + 1 < n_ && h_[i + 1] == '/' && !(i + 2 < n_ && is_alpha(h_[i + 2]))) {
                    // "</" not followed by a letter: bogus comment up to '>'
                    const std::size_t gt = h_.find('>', i + 2);
                    i = (gt == std::string::npos) ? n_ : gt + 1;
                    continue;
                }
                if (i + 1 < n_ && (is_alpha(h_[i + 1]) || h_[i + 1] == '/')) {
                    Tag t;
                    parse_tag(i, true, t);
                    if (!t.terminated) {
                        i = n_;  // unterminated tag at EOF is dropped
                        break;
                    }
                    i = t.next;
                    handle_tag(t, i);
                    continue;
                }
                out_.put_cp('<', pre_ > 0);
                ++i;
                continue;
            }
            if (ch == '&') {
                std::size_t end = 0;
                std::uint32_t cp = 0;
                if (decode_entity(h_, i, n_, end, cp)) {
                    out_.put_cp(cp, pre_ > 0);
                    i = end;
                } else {
                    out_.put_cp('&', pre_ > 0);
                    ++i;
                }
                continue;
            }
            const std::uint32_t cp = utf8_next(h_, i);
            out_.put_cp(cp, pre_ > 0);
        }
        finish_link();
        truncated = out_.truncated();
        return out_.finish();
    }

private:
    // Returns the index after a comment / CDATA / doctype / PI starting at p, or npos if p is not one.
    std::size_t skip_special(std::size_t p) const {
        if (p + 1 >= n_ || h_[p] != '<') return std::string::npos;
        if (h_[p + 1] == '!') {
            if (h_.compare(p, 4, "<!--") == 0) {
                // HTML5: "<!-->" and "<!--->" are complete, empty comments (abrupt closing); otherwise the comment
                // ends at the first "-->" or "--!>".
                if (p + 4 < n_ && h_[p + 4] == '>') return p + 5;
                if (p + 5 < n_ && h_[p + 4] == '-' && h_[p + 5] == '>') return p + 6;
                const std::size_t e1 = h_.find("-->", p + 4);
                const std::size_t e2 = h_.find("--!>", p + 4);
                if (e1 == std::string::npos && e2 == std::string::npos) return n_;
                if (e2 == std::string::npos || (e1 != std::string::npos && e1 < e2)) return e1 + 3;
                return e2 + 4;
            }
            if (h_.compare(p, 9, "<![CDATA[") == 0) {
                const std::size_t e = h_.find("]]>", p + 9);
                return e == std::string::npos ? n_ : e + 3;
            }
            const std::size_t e = h_.find('>', p + 2);
            return e == std::string::npos ? n_ : e + 1;
        }
        if (h_[p + 1] == '?') {
            const std::size_t e = h_.find('>', p + 2);
            return e == std::string::npos ? n_ : e + 1;
        }
        return std::string::npos;
    }

    // Parses the tag starting at h_[i] == '<' (followed by '/' or a letter).
    void parse_tag(std::size_t i, bool want_href, Tag& t) const {
        std::size_t p = i + 1;
        if (h_[p] == '/') {
            t.end = true;
            ++p;
        }
        const std::size_t name_start = p;
        while (p < n_ && !is_ws(h_[p]) && h_[p] != '/' && h_[p] != '>') ++p;
        const std::size_t name_len = (p - name_start < kMaxTagName) ? p - name_start : kMaxTagName;
        t.name.reserve(name_len);
        for (std::size_t k = 0; k < name_len; ++k) t.name.push_back(lc(h_[name_start + k]));

        for (;;) {
            while (p < n_ && (is_ws(h_[p]) || h_[p] == '/')) ++p;
            if (p >= n_) {
                t.terminated = false;
                t.next = n_;
                return;
            }
            if (h_[p] == '>') {
                t.self_closing = h_[p - 1] == '/';
                t.terminated = true;
                t.next = p + 1;
                return;
            }
            const std::size_t attr_start = p;
            if (h_[p] == '=') ++p;
            while (p < n_ && !is_ws(h_[p]) && h_[p] != '/' && h_[p] != '>' && h_[p] != '=') ++p;
            const bool is_href = (p - attr_start == 4) && ci_match(h_, attr_start, "href");
            while (p < n_ && is_ws(h_[p])) ++p;
            if (p < n_ && h_[p] == '=') {
                ++p;
                while (p < n_ && is_ws(h_[p])) ++p;
                std::size_t vs, ve;
                if (p < n_ && (h_[p] == '"' || h_[p] == '\'')) {
                    const char q = h_[p++];
                    vs = p;
                    while (p < n_ && h_[p] != q) ++p;
                    ve = p;
                    if (p >= n_) {  // unterminated quoted value => unterminated tag
                        t.terminated = false;
                        t.next = n_;
                        return;
                    }
                    ++p;
                } else {
                    vs = p;
                    while (p < n_ && !is_ws(h_[p]) && h_[p] != '>') ++p;
                    ve = p;
                }
                if (is_href && want_href && !t.has_href && ve - vs <= kMaxHrefRaw) {
                    t.href.assign(h_, vs, ve - vs);
                    t.has_href = true;
                }
            }
        }
    }

    // i is just after a raw-text start tag. Returns the index after its end tag (or n_).
    std::size_t skip_raw(std::size_t i, const std::string& name) const {
        std::size_t pos = i;
        while ((pos = h_.find('<', pos)) != std::string::npos) {
            if (pos + 1 < n_ && h_[pos + 1] == '/' && ci_match(h_, pos + 2, name)) {
                const std::size_t e = pos + 2 + name.size();
                if (e >= n_ || is_ws(h_[e]) || h_[e] == '/' || h_[e] == '>') {
                    const std::size_t gt = h_.find('>', e);
                    return gt == std::string::npos ? n_ : gt + 1;
                }
            }
            ++pos;
        }
        return n_;
    }

    // i is just after the start tag of a dropped element. Returns the index after its matching end tag.
    std::size_t skip_element(std::size_t i, const std::string& name) const {
        int depth = 1;
        std::size_t p = i;
        const bool stop_at_body = (name == "head");
        while (p < n_) {
            const std::size_t lt = h_.find('<', p);
            if (lt == std::string::npos) return n_;
            p = lt;
            const std::size_t sp = skip_special(p);
            if (sp != std::string::npos) {
                p = sp;
                continue;
            }
            if (p + 1 < n_ && (h_[p + 1] == '/' || is_alpha(h_[p + 1]))) {
                if (h_[p + 1] == '/' && !(p + 2 < n_ && is_alpha(h_[p + 2]))) {
                    ++p;
                    continue;
                }
                Tag t;
                parse_tag(p, false, t);
                if (!t.terminated) return n_;
                if (t.name == name) {
                    if (!t.end) {
                        if (!t.self_closing) ++depth;
                    } else if (--depth == 0) {
                        return t.next;
                    }
                } else if (!t.end && is_raw_text_name(t.name)) {
                    p = skip_raw(t.next, t.name);
                    continue;
                } else if (stop_at_body && !t.end && t.name == "body") {
                    return p;  // an unclosed <head> ends where <body> begins; do not consume it
                }
                p = t.next;
            } else {
                ++p;
            }
        }
        return n_;
    }

    void handle_tag(const Tag& t, std::size_t& i) {
        const std::string& nm = t.name;
        if (!t.end) {
            if (is_raw_text_name(nm)) {
                i = skip_raw(i, nm);
                return;
            }
            if (is_drop_name(nm)) {
                if (!t.self_closing) i = skip_element(i, nm);
                return;
            }
        }
        if (nm == "br") {
            out_.hard_newline();
        } else if (nm == "a") {
            finish_link();
            if (!t.end && t.has_href) start_link(t.href);
        } else if (nm == "pre") {
            if (!t.end) {
                out_.ensure_blank();
                ++pre_;
            } else {
                if (pre_ > 0) --pre_;
                out_.ensure_blank();
            }
        } else if (nm == "ul" || nm == "ol") {
            if (!t.end) {
                if (list_depth_ == 0) out_.ensure_blank(); else out_.ensure_newline();
                if (list_depth_ < kMaxListDepth) {
                    list_ordered_[list_depth_] = (nm == "ol");
                    list_count_[list_depth_] = 0;
                }
                ++list_depth_;
            } else {
                if (list_depth_ > 0) --list_depth_;
                if (list_depth_ == 0) out_.ensure_blank(); else out_.ensure_newline();
            }
        } else if (nm == "li") {
            out_.ensure_newline();
            if (!t.end) start_list_item();
        } else if (nm == "td" || nm == "th") {
            out_.space();
        } else if (nm == "hr") {
            out_.ensure_newline();
            out_.word("----");
            out_.ensure_newline();
        } else if (is_paragraph_name(nm)) {
            out_.ensure_blank();
        } else if (is_line_name(nm)) {
            out_.ensure_newline();
        }
        // Everything else (img, link, meta, base, span, b, i, embed, ...) contributes no markup.
    }

    void start_list_item() {
        int depth = list_depth_ > 0 ? list_depth_ : 1;
        int indent = (depth - 1) * 2;
        if (indent > 20) indent = 20;
        static const char kSpaces[] = "                    ";
        if (indent > 0) out_.literal(kSpaces, static_cast<std::size_t>(indent));
        char buf[24];
        if (list_depth_ > 0 && list_depth_ <= kMaxListDepth && list_ordered_[list_depth_ - 1]) {
            int& c = list_count_[list_depth_ - 1];
            if (c < 99999) ++c;
            const int len = std::snprintf(buf, sizeof buf, "%d. ", c);
            out_.literal(buf, static_cast<std::size_t>(len));
        } else {
            out_.literal("- ", 2);
        }
    }

    // Returns "" if the href is not an acceptable http(s)/mailto URL.
    static std::string clean_url(const std::string& raw) {
        std::string dec;
        dec.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size();) {
            if (raw[i] == '&') {
                std::size_t end = 0;
                std::uint32_t cp = 0;
                if (decode_entity(raw, i, raw.size(), end, cp)) {
                    if (cp < 0x80) {
                        dec.push_back(static_cast<char>(cp));
                    } else {
                        utf8_append(dec, cp);
                    }
                    i = end;
                    continue;
                }
            }
            dec.push_back(raw[i++]);
        }
        // Strip leading/trailing whitespace and every control character / space (browsers ignore tabs and newlines).
        std::string url;
        url.reserve(dec.size());
        for (char c : dec) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u <= 0x20 || u == 0x7F) continue;
            if (u >= 0x80 || c == '<' || c == '>' || c == '"') {
                char buf[4];
                std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned>(u));
                url += buf;
            } else {
                url.push_back(c);
            }
            if (url.size() > kMaxUrlOut) return std::string();
        }
        std::size_t prefix = 0;
        if (starts_with_ci(url, "https://")) prefix = 8;
        else if (starts_with_ci(url, "http://")) prefix = 7;
        else if (starts_with_ci(url, "mailto:")) prefix = 7;
        if (prefix == 0 || url.size() <= prefix) return std::string();
        return url;
    }

    void start_link(const std::string& raw_href) {
        std::string url = clean_url(raw_href);
        if (url.empty()) return;
        link_active_ = true;
        link_url_ = std::move(url);
        link_start_ = out_.size();
    }

    void finish_link() {
        if (!link_active_) return;
        link_active_ = false;
        const std::string& cur = out_.str();
        const std::size_t start = link_start_ <= cur.size() ? link_start_ : cur.size();
        std::string text = cur.substr(start);
        // Trim for comparison
        std::size_t b = 0;
        while (b < text.size() && (text[b] == ' ' || text[b] == '\n')) ++b;
        text.erase(0, b);
        const bool has_text = !text.empty();
        bool duplicate = false;
        if (has_text) {
            duplicate = (text == link_url_);
            if (!duplicate && starts_with_ci(link_url_, "mailto:") && text == link_url_.substr(7)) duplicate = true;
        }
        if (!duplicate) {
            if (has_text) out_.space();
            out_.word("<" + link_url_ + ">");
        }
        link_url_.clear();
    }

    const std::string& h_;
    std::size_t n_;
    TextOut out_;
    int pre_ = 0;
    int list_depth_ = 0;
    bool list_ordered_[kMaxListDepth] = {};
    int list_count_[kMaxListDepth] = {};
    bool link_active_ = false;
    std::string link_url_;
    std::size_t link_start_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string html_to_text(const std::string& html, std::size_t max_chars, bool& truncated) {
    truncated = false;
    HtmlConverter c(html, max_chars);
    return c.run(truncated);
}

std::string sanitize_text(const std::string& text, std::size_t max_chars, bool& truncated) {
    truncated = false;
    std::string out;
    out.reserve((text.size() < max_chars ? text.size() : max_chars) + 8);
    std::size_t i = 0;
    while (i < text.size()) {
        std::uint32_t cp = utf8_next(text, i);
        if (cp == '\r') {
            if (i < text.size() && text[i] == '\n') ++i;
            cp = '\n';
        } else if (is_line_break_cp(cp) || cp == '\f') {
            cp = '\n';
        }
        if (is_stripped(cp)) continue;
        if (out.size() + utf8_len(cp) > max_chars) {
            truncated = true;
            break;
        }
        utf8_append(out, cp);
    }
    return out;
}

std::string sanitize_line(const std::string& s, std::size_t max_chars) {
    std::string out;
    out.reserve((s.size() < max_chars ? s.size() : max_chars) + 4);
    bool pending_space = false;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::uint32_t cp = utf8_next(s, i);
        if (is_stripped(cp)) continue;
        if (is_space_cp(cp)) {
            pending_space = !out.empty();
            continue;
        }
        const std::size_t need = utf8_len(cp) + (pending_space ? 1 : 0);
        if (out.size() + need > max_chars) break;
        if (pending_space) out.push_back(' ');
        pending_space = false;
        utf8_append(out, cp);
    }
    return out;
}

std::string utf8_truncate(const std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

namespace {

std::string sanitize_mime(const std::string& raw) {
    const std::string line = sanitize_line(raw, 255);
    std::string tok;
    for (char c : line) {
        if (c == ' ' || c == ';') break;
        tok.push_back(lc(c));
    }
    // type "/" subtype, restricted token characters (RFC 6838)
    std::size_t slash = tok.find('/');
    bool ok = slash != std::string::npos && slash > 0 && slash + 1 < tok.size() && tok.size() <= 127 &&
              tok.find('/', slash + 1) == std::string::npos;
    if (ok) {
        for (char c : tok) {
            if (c == '/') continue;
            const bool good = is_alnum(c) || c == '.' || c == '+' || c == '-' || c == '_' || c == '!' || c == '#' ||
                              c == '$' || c == '&' || c == '^';
            if (!good) {
                ok = false;
                break;
            }
        }
    }
    return ok ? tok : std::string("application/octet-stream");
}

} // namespace

AttachmentMeta sanitize_attachment(const AttachmentMeta& raw) {
    AttachmentMeta a;
    a.id = sanitize_line(raw.id, 512);
    a.name = sanitize_line(raw.name, Limits::kMaxAttachmentName);
    if (a.name.empty()) a.name = "(unnamed)";
    a.content_type = sanitize_mime(raw.content_type);
    a.size = raw.size < 0 ? -1 : raw.size;
    a.is_inline = raw.is_inline;
    switch (raw.kind) {
    case AttachmentKind::File:
    case AttachmentKind::Item:
    case AttachmentKind::Reference:
    case AttachmentKind::Unknown:
        a.kind = raw.kind;
        break;
    default:
        a.kind = AttachmentKind::Unknown;
        break;
    }
    return a;
}

std::vector<AttachmentMeta> sanitize_attachments(const std::vector<AttachmentMeta>& raw) {
    std::vector<AttachmentMeta> out;
    const std::size_t n = raw.size() < Limits::kMaxAttachmentsListed ? raw.size() : Limits::kMaxAttachmentsListed;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(sanitize_attachment(raw[i]));
    return out;
}

std::string human_size(std::int64_t bytes) {
    if (bytes < 0) return "unknown size";
    if (bytes < 1024) return std::to_string(bytes) + " B";
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1023.5 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (v < 9.95) {
        std::snprintf(buf, sizeof buf, "%.1f", v);
        std::string s = buf;
        if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
        return s + " " + kUnits[unit];
    }
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return std::string(buf) + " " + kUnits[unit];
}

} // namespace slowmail::core
