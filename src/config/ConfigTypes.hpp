#ifndef CONFIG_TYPES_HPP
#define CONFIG_TYPES_HPP

#include <string>
#include <vector>
#include <map>

struct LocationConfig
{
    std::string                        path;             // e.g. "/", "/uploads"
    std::vector<std::string>           methods;          // allowed HTTP methods for this route
    std::string                        root;             // filesystem directory this location maps to
    std::string                        index;            // default file served for a directory request
    bool                                autoindex;        // directory listing on/off
    std::string                        upload_store;     // dir uploads get written to (empty = uploads disabled)
    std::map<std::string, std::string> cgi_ext;           // extension -> interpreter path, e.g. ".py" -> "/usr/bin/python3"
    bool                                has_redirect;
    int                                 redirect_code;    // e.g. 301
    std::string                        redirect_target;
    bool                                has_max_body;     // set -> override server's cap
    size_t                              client_max_body_size;

    LocationConfig() : autoindex(false), has_redirect(false), redirect_code(0),
                       has_max_body(false), client_max_body_size(0) {}
};

struct ServerConfig
{
    std::string                 host;                  // e.g. "127.0.0.1"
    int                          port;                   // e.g. 8080
    std::string                 server_name;            // optional (virtual hosts out of scope, but stored anyway)
    std::map<int, std::string>  error_pages;            // status code -> path to custom error page
    size_t                       client_max_body_size;   // in bytes
    std::vector<LocationConfig> locations;

    ServerConfig() : port(0), client_max_body_size(1 * 1024 * 1024) {}
};

#endif
