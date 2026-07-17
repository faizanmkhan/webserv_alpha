#include "Router.hpp"

const LocationConfig *matchLocation(const ServerConfig &srv, const std::string &path)
{
    const LocationConfig *best = NULL;

    for (size_t i = 0; i < srv.locations.size(); ++i)
    {
        const LocationConfig &loc = srv.locations[i];

        // is loc.path a prefix of the request path?
        if (path.compare(0, loc.path.size(), loc.path) == 0)
        {
            // keep it only if it's the first match, or longer than the best so far
            if (best == NULL || loc.path.size() > best->path.size())
                best = &srv.locations[i];
        }
    }
    return best;
}
