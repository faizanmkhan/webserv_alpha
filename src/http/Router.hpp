#ifndef ROUTER_HPP
#define ROUTER_HPP

#include "../config/ConfigTypes.hpp"
#include <string>

// Returns the best (longest-prefix) matching location, or NULL if none match.
const LocationConfig *matchLocation(const ServerConfig &srv, const std::string &path);

#endif