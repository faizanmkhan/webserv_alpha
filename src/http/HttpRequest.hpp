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

// Parse the request line + header block. Returns 0 on success, or an HTTP
// error code to answer with: 400 (malformed), 414 (request line too long),
// 505 (unsupported HTTP version).
int parseRequest(const std::string &raw, HttpRequest &out);

// Decode a Transfer-Encoding: chunked body. `raw` is the bytes after the header
// block. On CHUNK_DONE `out` holds the un-chunked body. CHUNK_INCOMPLETE means
// wait for more bytes; CHUNK_ERROR means malformed (answer 400).
enum ChunkStatus { CHUNK_INCOMPLETE, CHUNK_DONE, CHUNK_ERROR };
ChunkStatus decodeChunked(const std::string &raw, std::string &out);

#endif
