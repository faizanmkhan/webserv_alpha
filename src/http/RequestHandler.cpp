#include "RequestHandler.hpp"
#include "Router.hpp"

static std::string buildResponse(int code, const std::string &reason,
                                 const std::string &contentType,
                                 const std::string &body)
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << reason << "\r\n";
    oss << "Content-Type: " << contentType << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    return oss.str();
}

std::string errorResponse(int code, const std::string &reason)
{
    std::ostringstream body;
    body << "<html><body><h1>" << code << " " << reason << "</h1></body></html>";
    return buildResponse(code, reason, "text/html", body.str());
}

static std::string noContentResponse()
{
    return "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
}

static std::string createdResponse(const std::string &location)
{
    std::string body = "<html><body><h1>201 Created</h1></body></html>";
    std::ostringstream oss;
    oss << "HTTP/1.1 201 Created\r\n";
    oss << "Content-Type: text/html\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Location: " << location << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    return oss.str();
}

static std::string redirectResponse(int code, const std::string &location)
{
    std::string reason = (code == 301) ? "Moved Permanently" : "Found";
    std::ostringstream oss;
    oss << "HTTP/1.1 " << code << " " << reason << "\r\n";
    oss << "Location: " << location << "\r\n";
    oss << "Content-Length: 0\r\n";
    oss << "Connection: close\r\n\r\n";
    return oss.str();
}

static std::string contentTypeFor(const std::string &path)
{
    size_t dot = path.rfind('.');
    if (dot == std::string::npos)
        return "application/octet-stream";
    std::string ext = path.substr(dot);
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css")  return "text/css";
    if (ext == ".js")   return "application/javascript";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")  return "image/gif";
    if (ext == ".txt")  return "text/plain";
    return "application/octet-stream";
}

static bool methodAllowed(const LocationConfig &loc, const std::string &method)
{
    for (size_t i = 0; i < loc.methods.size(); ++i)
        if (loc.methods[i] == method)
            return true;
    return false;
}

// A 405 must tell the client which methods ARE allowed here, via an Allow
// header listing exactly this location's configured methods.
static std::string methodNotAllowedResponse(const LocationConfig &loc)
{
    std::string allow;
    for (size_t i = 0; i < loc.methods.size(); ++i)
    {
        if (i > 0)
            allow += ", ";
        allow += loc.methods[i];
    }

    std::string body = "<html><body><h1>405 Method Not Allowed</h1></body></html>";

    std::ostringstream oss;
    oss << "HTTP/1.1 405 Method Not Allowed\r\n";
    oss << "Content-Type: text/html\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Allow: " << allow << "\r\n";
    oss << "Connection: close\r\n\r\n";
    oss << body;
    return oss.str();
}

// Read a whole regular file into `out`. Regular files are exempt from the
// non-blocking/epoll rule, so a synchronous read loop is allowed here.
static bool readFile(const std::string &path, std::string &out)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1)
        return false;
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(r));
    close(fd);
    return (r != -1);
}
// Write `data` to a regular file, creating/truncating it. Like readFile, a
// regular file is exempt from the epoll rule, so a synchronous loop is fine.
static bool writeFile(const std::string &path, const std::string &data)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1)
        return false;
    size_t total = 0;
    while (total < data.size())
    {
        ssize_t w = write(fd, data.c_str() + total, data.size() - total);
        if (w <= 0)                 // decision from return value only, no errno
        {
            close(fd);
            return false;
        }
        total += static_cast<size_t>(w);
    }
    close(fd);
    return true;
}

// Produce an error response, preferring a configured custom error_page file.
// The page path (e.g. "/errors/404.html") is resolved against the matched
// location's root; if there is no location, or the file can't be read, we fall
// back to the generated stub so a 4xx/5xx is never empty.
static std::string errorPage(const ServerConfig &srv, const LocationConfig *loc,
                             int code, const std::string &reason)
{
    std::map<int, std::string>::const_iterator it = srv.error_pages.find(code);
    if (it != srv.error_pages.end() && loc != NULL)
    {
        std::string fsPath = loc->root + it->second;
        std::string body;
        if (readFile(fsPath, body))
            return buildResponse(code, reason, contentTypeFor(fsPath), body);
    }
    return errorResponse(code, reason);
}

// Generate an HTML directory listing (used when a directory is requested,
// autoindex is on, and there is no index file to serve).
static std::string autoindexPage(const std::string &fsPath, const std::string &reqPath)
{
    DIR *dir = opendir(fsPath.c_str());
    if (dir == NULL)
        return errorResponse(403, "Forbidden");

    std::string base = reqPath;
    if (base.empty() || base[base.size() - 1] != '/')
        base += "/";   // makes every link absolute, with or without a trailing slash

    std::ostringstream html;
    html << "<!DOCTYPE html><html><head><title>Index of " << reqPath
         << "</title></head><body>\r\n";
    html << "<h1>Index of " << reqPath << "</h1><hr><ul>\r\n";

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        html << "<li><a href=\"" << base << name << "\">" << name << "</a></li>\r\n";
    }
    closedir(dir);

    html << "</ul><hr></body></html>\r\n";
    return buildResponse(200, "OK", "text/html", html.str());
}

static std::string serveGet(const ServerConfig &srv, const LocationConfig &loc,
                            const HttpRequest &req)
{
    std::string fsPath = loc.root + req.path;

    struct stat st;
    if (stat(fsPath.c_str(), &st) == -1)
        return errorPage(srv, &loc, 404, "Not Found");

    if (S_ISDIR(st.st_mode))
    {
        if (fsPath[fsPath.size() - 1] != '/')
            fsPath += "/";

        // Prefer a configured index file if it exists and is a regular file.
        struct stat ist;
        std::string indexPath = fsPath + loc.index;
        if (!loc.index.empty() && stat(indexPath.c_str(), &ist) == 0 && !S_ISDIR(ist.st_mode))
            fsPath = indexPath;                     // serve it (falls through below)
        else if (loc.autoindex)
            return autoindexPage(fsPath, req.path); // no index -> directory listing
        else
            return errorPage(srv, &loc, 403, "Forbidden");
    }

    std::string body;
    if (!readFile(fsPath, body))
        return errorPage(srv, &loc, 403, "Forbidden");

    return buildResponse(200, "OK", contentTypeFor(fsPath), body);
}

// Parse a multipart/form-data body: find the part carrying a file (the one
// with a filename="..." attribute) and extract its original filename and its
// raw bytes. Returns false if the body is malformed or carries no file part.
//
// A part looks like:
//   --BOUNDARY\r\n
//   Content-Disposition: form-data; name="file"; filename="photo.jpg"\r\n
//   Content-Type: image/jpeg\r\n
//   \r\n                      <- blank line: headers end here
//   <raw file bytes>\r\n
//   --BOUNDARY--\r\n
static bool parseMultipart(const std::string &body, const std::string &boundary,
                           std::string &filename, std::string &content)
{
    std::string delim = "--" + boundary;

    // Locate the file part via its filename= attribute.
    size_t fn = body.find("filename=\"");
    if (fn == std::string::npos)
        return false;
    fn += 10;                                 // strlen of  filename="
    size_t fnEnd = body.find('"', fn);
    if (fnEnd == std::string::npos)
        return false;
    filename = body.substr(fn, fnEnd - fn);
    if (filename.empty())
        return false;                         // form submitted with no file chosen

    // The file bytes start after this part's blank line (first \r\n\r\n after
    // the filename), and end right before the next boundary (\r\n--BOUNDARY).
    size_t start = body.find("\r\n\r\n", fnEnd);
    if (start == std::string::npos)
        return false;
    start += 4;

    size_t end = body.find("\r\n" + delim, start);
    if (end == std::string::npos)
        return false;

    content = body.substr(start, end - start);
    return true;
}

static std::string serveDelete(const ServerConfig &srv, const LocationConfig &loc,
                               const HttpRequest &req)
{
    std::string fsPath = loc.root + req.path;   // same mapping as GET

    struct stat st;
    if (stat(fsPath.c_str(), &st) == -1)
        return errorPage(srv, &loc, 404, "Not Found");
    if (S_ISDIR(st.st_mode))
        return errorPage(srv, &loc, 403, "Forbidden");   // no deleting directories
    if (access(fsPath.c_str(), W_OK) == -1)
        return errorPage(srv, &loc, 403, "Forbidden");   // not writable

    if (std::remove(fsPath.c_str()) != 0)
        return errorPage(srv, &loc, 403, "Forbidden");   // removal denied/failed

    return noContentResponse();                          // 204: gone
}

static std::string serveUpload(const ServerConfig &srv, const LocationConfig &loc,
                               const HttpRequest &req)
{
    // Uploads disabled on this route (no upload_store configured).
    if (loc.upload_store.empty())
        return errorPage(srv, &loc, 403, "Forbidden");

    std::string filename;
    std::string content;

    // Browser form upload (multipart) vs. raw body (curl --data-binary)?
    std::map<std::string, std::string>::const_iterator ct =
        req.headers.find("content-type");
    if (ct != req.headers.end() &&
        ct->second.find("multipart/form-data") != std::string::npos)
    {
        // Pull boundary=... out of the Content-Type value.
        size_t b = ct->second.find("boundary=");
        if (b == std::string::npos)
            return errorPage(srv, &loc, 400, "Bad Request");
        std::string boundary = ct->second.substr(b + 9);
        size_t stop = boundary.find_first_of("; \t\r\n");   // trim any trailing params
        if (stop != std::string::npos)
            boundary = boundary.substr(0, stop);

        if (!parseMultipart(req.body, boundary, filename, content))
            return errorPage(srv, &loc, 400, "Bad Request");
    }
    else
    {
        // Raw upload: filename from the URL path, content is the whole body.
        size_t slash = req.path.rfind('/');
        filename = (slash == std::string::npos)
                   ? req.path : req.path.substr(slash + 1);
        content = req.body;
    }

    // The filename may come from the client (multipart) — keep only the
    // basename so an uploaded name like "../../x" cannot escape upload_store.
    size_t s2 = filename.rfind('/');
    if (s2 != std::string::npos)
        filename = filename.substr(s2 + 1);
    if (filename.empty() || filename == "..")
        return errorPage(srv, &loc, 400, "Bad Request");

    std::string dest = loc.upload_store + "/" + filename;
    if (!writeFile(dest, content))
        return errorPage(srv, &loc, 500, "Internal Server Error");

    return createdResponse(loc.path + "/" + filename);   // where to GET it back
}

std::string handleRequest(const ServerConfig &srv, const HttpRequest &req)
{
    // Reject any path that tries to climb out of the configured root.
    if (req.path.find("..") != std::string::npos)
        return errorPage(srv, NULL, 403, "Forbidden");

    const LocationConfig *loc = matchLocation(srv, req.path);
    if (loc == NULL)
        return errorPage(srv, NULL, 404, "Not Found");

    if (loc->has_redirect)
        return redirectResponse(loc->redirect_code, loc->redirect_target);

    if (!methodAllowed(*loc, req.method))
        return methodNotAllowedResponse(*loc);

    if (req.method == "GET")
        return serveGet(srv, *loc, req);

    if (req.method == "POST")
        return serveUpload(srv, *loc, req);

    if (req.method == "DELETE")
        return serveDelete(srv, *loc, req);

    return errorPage(srv, loc, 501, "Not Implemented");
}
