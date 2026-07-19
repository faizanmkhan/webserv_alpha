#include "CgiHandler.hpp"

// Turn a header name like "User-Agent" into "HTTP_USER_AGENT".
static std::string toEnvName(const std::string &h)
{
    std::string s = "HTTP_";
    for (size_t i = 0; i < h.size(); ++i)
        s += (h[i] == '-') ? '_' : static_cast<char>(std::toupper(h[i]));
    return s;
}

static std::vector<std::string> buildCgiEnv(const ServerConfig &srv,
                                            const HttpRequest &req,
                                            const std::string &scriptPath)
{
    std::vector<std::string> env;

    env.push_back("GATEWAY_INTERFACE=CGI/1.1");
    env.push_back("SERVER_PROTOCOL=HTTP/1.1");
    env.push_back("REQUEST_METHOD=" + req.method);
    env.push_back("SCRIPT_NAME=" + req.path);
    env.push_back("SCRIPT_FILENAME=" + scriptPath);
    env.push_back("PATH_INFO=" + req.path);
    // REQUEST_URI is the original request target (path + query). The 42
    // cgi_tester cross-checks it against SCRIPT_NAME/PATH_INFO and rejects the
    // request ("PATH_INFO incorrect") if it's missing.
    std::string requestUri = req.path;
    if (!req.query.empty())
        requestUri += "?" + req.query;
    env.push_back("REQUEST_URI=" + requestUri);
    env.push_back("QUERY_STRING=" + req.query);
    env.push_back("SERVER_NAME=" + srv.host);
    env.push_back("REMOTE_ADDR=127.0.0.1");   // we accept() with NULL addr

    std::ostringstream port;
    port << srv.port;
    env.push_back("SERVER_PORT=" + port.str());

    std::ostringstream clen;
    clen << req.body.size();
    env.push_back("CONTENT_LENGTH=" + clen.str());

    std::map<std::string, std::string>::const_iterator ct =
        req.headers.find("content-type");
    if (ct != req.headers.end())
        env.push_back("CONTENT_TYPE=" + ct->second);

    // Every other request header becomes HTTP_*.
    for (std::map<std::string, std::string>::const_iterator it =
             req.headers.begin(); it != req.headers.end(); ++it)
    {
        if (it->first == "content-type" || it->first == "content-length")
            continue;   // these get their own non-HTTP_ vars above
        env.push_back(toEnvName(it->first) + "=" + it->second);
    }
    return env;
}

bool startCgi(const ServerConfig &srv, const HttpRequest &req,
              const std::string &interpreter, const std::string &scriptPath,
              CgiProcess &proc)
{
    int pipeIn[2];    // parent -> child stdin
    int pipeOut[2];   // child stdout -> parent
    if (pipe(pipeIn) == -1)
        return false;
    if (pipe(pipeOut) == -1) {
        close(pipeIn[0]); close(pipeIn[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipeIn[0]); close(pipeIn[1]);
        close(pipeOut[0]); close(pipeOut[1]);
        return false;
    }

    if (pid == 0)
    {
        // ---- CHILD ----
        dup2(pipeIn[0], STDIN_FILENO);    // stdin  <- pipeIn read end
        dup2(pipeOut[1], STDOUT_FILENO);  // stdout -> pipeOut write end
        // Close every original end now that fds 0/1 point at the pipes.
        close(pipeIn[0]);  close(pipeIn[1]);
        close(pipeOut[0]); close(pipeOut[1]);

        // Run relative to the script's own directory (subject requirement).
        std::string dir = ".", base = scriptPath;
        size_t slash = scriptPath.rfind('/');
        if (slash != std::string::npos) {
            dir  = scriptPath.substr(0, slash);
            base = scriptPath.substr(slash + 1);
        }
        if (chdir(dir.c_str()) == -1)
            _exit(1);

        std::vector<std::string> envv = buildCgiEnv(srv, req, scriptPath);
        std::vector<char*> envp;
        for (size_t i = 0; i < envv.size(); ++i)
            envp.push_back(const_cast<char*>(envv[i].c_str()));
        envp.push_back(NULL);

        char* argv[] = { const_cast<char*>(interpreter.c_str()),
                         const_cast<char*>(base.c_str()), NULL };
        execve(interpreter.c_str(), argv, &envp[0]);
        _exit(1);   // execve only returns on failure
    }

    // ---- PARENT ----
    close(pipeIn[0]);    // parent never reads child's stdin pipe
    close(pipeOut[1]);   // parent never writes child's stdout pipe
    fcntl(pipeIn[1],  F_SETFL, O_NONBLOCK);
    fcntl(pipeOut[0], F_SETFL, O_NONBLOCK);

    proc.pid   = pid;
    proc.inFd  = pipeIn[1];
    proc.outFd = pipeOut[0];
    return true;
}
bool resolveCgi(const ServerConfig &srv, const HttpRequest &req,
                std::string &interpreter, std::string &scriptPath)
{
    const LocationConfig *loc = matchLocation(srv, req.path);
    if (loc == NULL || loc->cgi_ext.empty())
        return false;

    size_t dot = req.path.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = req.path.substr(dot);
    if (ext.find('/') != std::string::npos)   // the dot was in a directory name
        return false;

    std::map<std::string, std::string>::const_iterator it = loc->cgi_ext.find(ext);
    if (it == loc->cgi_ext.end())
        return false;

    interpreter = it->second;
    scriptPath  = loc->root + req.path;
    return true;
}

std::string buildCgiResponse(const std::string &raw)
{
    if (raw.empty())
        return errorResponse(502, "Bad Gateway");   // child produced nothing

    // Split the script's headers from its body at the first blank line.
    std::string sep = "\r\n\r\n";
    size_t pos = raw.find(sep);
    if (pos == std::string::npos) { sep = "\n\n"; pos = raw.find(sep); }

    std::string head = (pos == std::string::npos) ? std::string() : raw.substr(0, pos);
    std::string body = (pos == std::string::npos) ? raw : raw.substr(pos + sep.size());

    int         code = 200;
    std::string reason = "OK";
    std::string passthrough;
    bool        hasType = false;

    std::istringstream hs(head);
    std::string line;
    while (std::getline(hs, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        if (line.empty())
            continue;

        size_t colon = line.find(':');
        std::string key = (colon == std::string::npos) ? line : line.substr(0, colon);
        for (size_t i = 0; i < key.size(); ++i)
            key[i] = static_cast<char>(std::tolower(key[i]));

        if (key == "status")                       // e.g. "Status: 404 Not Found"
        {
            std::istringstream ss(line.substr(colon + 1));
            ss >> code;
            std::getline(ss, reason);
            if (!reason.empty() && reason[0] == ' ') reason.erase(0, 1);
            if (reason.empty()) reason = "OK";
            continue;                              // Status is not a real HTTP header
        }
        // We set the framing headers ourselves, so drop the script's copies to
        // avoid a duplicate/conflicting Content-Length (RFC 7230 3.3.2) — a
        // correct CGI (e.g. the 42 cgi_tester) sends its own Content-Length.
        if (key == "content-length" || key == "transfer-encoding" ||
            key == "connection")
            continue;
        if (key == "content-type")
            hasType = true;
        passthrough += line + "\r\n";              // forward every other header as-is
    }

    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << reason << "\r\n";
    out << passthrough;
    if (!hasType)
        out << "Content-Type: text/html\r\n";
    out << "Content-Length: " << body.size() << "\r\n";   // we set it, script omits it
    out << "Connection: close\r\n\r\n";
    out << body;
    return out.str();
}
