#pragma once
// Serves one staged .pkg to the PS5 installer over HTTP byte ranges, so a package
// can be installed where it already sits instead of being copied first.

#include <cstdint>
#include <string>

namespace prospero {

// Serves a local file, and returns the URI to hand the installer.
bool StartPkgStream(const std::string &path, std::string *uri, std::string *err);

// Serves a remote package by translating the installer's seeks into Range requests,
// so nothing is staged on the console. Fails if the server can't be read in ranges.
bool StartRemotePkgStream(const std::string &url, std::string *uri, std::string *err);

// Serves a live RAM session (pkg_live_cache.hpp): no file, no URL — bytes come from the
// browser as the installer reads them. Fails if the session isn't active.
bool StartLivePkgStream(const std::string &session_id, std::string *uri, std::string *err);

// Drops the session behind `uri`; later requests get 404. Takes the uri, not the
// newest session, which may belong to another install still streaming.
void StopPkgStream(const std::string &uri);

// Keeps one PNG for the console's install card and returns the URL it is fetched from.
// An empty or non-PNG image clears it and returns an empty string.
std::string SetPkgCover(const std::string &png);

// Total bytes the stream server has sent to the installer since startup, for rate logging.
uint64_t PkgStreamBytesServed();

} // namespace prospero
