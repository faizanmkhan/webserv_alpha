#ifndef HTTP_REQUEST_HPP
#define HTTP_REQUEST_HPP

#include <string>
#include <map>

struct HttpRequest
{
    std::string                        method;    // "GET"
    std::string                        path;      // decoded path only, e.g. "/uploads/foo.png"
    std::string                        query;     // everything after '?', raw (for CGI QUERY_STRING)
    std::string                        version;   // "HTTP/1.1"
    std::map<std::string, std::string> headers;
    std::string                        body;      // raw request body bytes
};

bool parseRequest(const std::string &raw, HttpRequest &out);

#endif
