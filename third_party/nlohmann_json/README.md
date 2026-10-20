# Vendored nlohmann/json (header-only, MIT)

Copied verbatim from Ubuntu 24.04 package `nlohmann-json3-dev` 3.11.3 so the
reMarkable SDK cross-build (whose sysroot has no nlohmann/json) is self-contained.
The top-level CMakeLists.txt prefers a system `find_package(nlohmann_json)` and
falls back to this copy. Upstream: https://github.com/nlohmann/json
