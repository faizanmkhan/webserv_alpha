#include "RequestHandler.hpp"
#include "Router.hpp"
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

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

    // POST and DELETE bodies are handled in a later phase.
    return errorPage(srv, loc, 501, "Not Implemented");
}
