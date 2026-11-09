#pragma once
// Routes for the Media page: lists, streams and thumbnails the console's screenshots and clips.

#include "config.hpp"
#include "http_server.hpp"

namespace prospero {

void RegisterMediaRoutes(HttpServer *server, const Config &cfg);

} // namespace prospero
