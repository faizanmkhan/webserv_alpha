#ifndef REQUEST_HANDLER_HPP
#define REQUEST_HANDLER_HPP

#include "../config/ConfigTypes.hpp"
#include "HttpRequest.hpp"
#include <string>

// Build the full HTTP response (status line + headers + body) for a request.
std::string handleRequest(const ServerConfig &srv, const HttpRequest &req);

// Build a minimal HTML error response. Exposed so the event loop can answer 400
// (malformed request) before it ever has a parsed HttpRequest to hand us.
std::string errorResponse(int code, const std::string &reason);

#endif
