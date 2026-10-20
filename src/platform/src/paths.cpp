// Data directory resolution and device detection (see paths.hpp).
#include "slowmail/platform/paths.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace slowmail::platform {

namespace {

constexpr const char* kDeviceDataDir = "/home/root/.local/share/slowmail";

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains_icase(const std::string& haystack, const std::string& needle) {
    auto lower = [](std::string s) {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

bool is_dir(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_os_release_remarkable() {
    std::istringstream in(read_file("/etc/os-release"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("ID=", 0) == 0 || line.rfind("ID_LIKE=", 0) == 0) {
            if (contains_icase(line, "remarkable")) return true;
        }
    }
    return false;
}

} // namespace

bool is_remarkable_device() {
    if (contains_icase(read_file("/sys/devices/soc0/machine"), "remarkable")) return true;
    if (is_os_release_remarkable()) return true;
    return ::access("/usr/bin/xochitl", F_OK) == 0;
}

std::string data_dir() {
    const std::string override_dir = env_or_empty("SLOWMAIL_DATA_DIR");
    if (!override_dir.empty()) return override_dir;
    if (is_remarkable_device()) return kDeviceDataDir;

    const std::string xdg = env_or_empty("XDG_DATA_HOME");
    if (!xdg.empty()) return xdg + "/slowmail";
    std::string home = env_or_empty("HOME");
    if (home.empty()) home = "/tmp";
    return home + "/.local/share/slowmail";
}

std::string database_path() {
    return data_dir() + "/slowmail.sqlite3";
}

bool ensure_dir(const std::string& path, int mode) {
    if (path.empty()) return false;
    if (is_dir(path)) return true;

    // mkdir -p: create each missing component in turn.
    const mode_t m = static_cast<mode_t>(mode);
    std::string current;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t next = path.find('/', pos);
        const std::size_t end = (next == std::string::npos) ? path.size() : next;
        current = path.substr(0, end);
        if (!current.empty() && !is_dir(current)) {
            if (::mkdir(current.c_str(), m) != 0 && errno != EEXIST) return false;
            if (!is_dir(current)) return false;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    // Tighten the leaf even if umask or a prior creation was more permissive; ignore failure for foreign dirs.
    (void)::chmod(path.c_str(), m);
    return is_dir(path);
}

} // namespace slowmail::platform
