#pragma once
#include <string>

namespace slowmail::platform {

// Where Slow Mail keeps its data. Device: /home/root/.local/share/slowmail (reMarkable OS has no XDG dirs by default).
// Host: $XDG_DATA_HOME/slowmail or ~/.local/share/slowmail. Override with SLOWMAIL_DATA_DIR.
std::string data_dir();
std::string database_path();   // data_dir()/slowmail.sqlite3
bool ensure_dir(const std::string& path, int mode = 0700);

// True when running on a reMarkable device (checks /etc/os-release or /sys/devices/soc0/machine).
bool is_remarkable_device();

} // namespace slowmail::platform
