#include "HttpRequest.hpp"
#include <cctype>

static bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// Turn "%20" into a space, "%2e" into '.', etc. A lone '%' or a '%' not
// followed by two hex digits is left untouched.
static std::string percentDecode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && isHexDigit(s[i + 1]) && isHexDigit(s[i + 2]))
        {
            out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        }
        else
            out += s[i];
    }
    return out;
}

// Cap the request line so a client can't force unbounded buffering with one
// enormous URI (answered 414 rather than growing memory without limit).
#define MAX_REQUEST_LINE 8192

int parseRequest(const std::string &raw, HttpRequest &out)
{
    // request line: "GET /path HTTP/1.1\r\n"
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos)
        return 400;
    std::string line = raw.substr(0, lineEnd);
    if (line.size() > MAX_REQUEST_LINE)
        return 414;

    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos)
        return 400;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos)
        return 400;
    if (line.find(' ', sp2 + 1) != std::string::npos)
        return 400;

    out.method  = line.substr(0, sp1);
    out.version = line.substr(sp2 + 1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (out.method.empty() || target.empty() || out.version.empty())
        return 400;
    // We speak HTTP/1.1; accept 1.0 too, reject anything else.
    if (out.version != "HTTP/1.1" && out.version != "HTTP/1.0")
        return 505;

    // Split "path?query", then percent-decode ONLY the path. Decoding here,
    // before routing and the traversal check, is deliberate: we always
    // validate the decoded path, never the raw one.
    size_t q = target.find('?');
    if (q != std::string::npos)
    {
        out.query = target.substr(q + 1);
        target = target.substr(0, q);
    }
    out.path = percentDecode(target);
    if (out.path.empty())
        return 400;

    size_t pos = lineEnd + 2;
    while (pos < raw.size())
    {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos)
            return 400;
        std::string hline = raw.substr(pos, end - pos);
        size_t colon = hline.find(':');
        if (colon == std::string::npos)
            return 400;
        std::string name  = hline.substr(0, colon);
        std::string value = hline.substr(colon + 1);
        for (size_t i = 0; i < name.size(); ++i)
            name[i] = std::tolower(static_cast<unsigned char>(name[i]));
        size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos)
            value = "";
        else
        {
            size_t last = value.find_last_not_of(" \t");
            value = value.substr(first, last - first + 1);
        }
        out.headers[name] = value;
        pos = end + 2;
    }
    return 0;
}

// Parse a chunk-size line (hexadecimal, possibly with a ";ext"). Returns false
// on an empty or non-hex size.
static bool parseChunkSize(const std::string &line, unsigned long &val)
{
    std::string s = line;
    size_t semi = s.find(';');          // strip any chunk extension
    if (semi != std::string::npos)
        s = s.substr(0, semi);
    if (s.empty())
        return false;
    val = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (!isHexDigit(s[i]))
            return false;
        val = val * 16 + hexValue(s[i]);
    }
    return true;
}

ChunkStatus decodeChunked(const std::string &raw, size_t &pos, std::string &out)
{
    while (true)
    {
        size_t lineEnd = raw.find("\r\n", pos);
        if (lineEnd == std::string::npos)
            return CHUNK_INCOMPLETE;                 // size line not fully here

        unsigned long chunkSize;
        if (!parseChunkSize(raw.substr(pos, lineEnd - pos), chunkSize))
            return CHUNK_ERROR;                      // malformed size

        size_t dataStart = lineEnd + 2;
        if (chunkSize == 0)                          // final chunk
        {
            if (raw.size() < dataStart + 2)          // need terminating CRLF
                return CHUNK_INCOMPLETE;
            pos = dataStart + 2;
            return CHUNK_DONE;
        }
        if (raw.size() < dataStart + chunkSize + 2)  // data + its CRLF not all here
            return CHUNK_INCOMPLETE;                 // pos stays: retry this chunk

        out.append(raw, dataStart, chunkSize);
        pos = dataStart + chunkSize + 2;             // consumed: never look back
    }
}
