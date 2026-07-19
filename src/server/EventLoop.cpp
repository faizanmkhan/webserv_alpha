#include "EventLoop.hpp"
#include "../http/CgiHandler.hpp"
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctime>
#include <csignal>

#define CGI_TIMEOUT 5   // seconds a CGI child may run before we kill it -> 504

// Flip a client fd from "I want to read" to "I want to write". We call this
// once a full response has been built into writeBuffer, so the next time the
// socket is writable epoll wakes us on EPOLLOUT and we send it.
static void flipToWrite(int epfd, int fd)
{
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events  = EPOLLOUT;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void epollAdd(int epfd, int fd, unsigned int events)
{
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

static void epollMod(int epfd, int fd, unsigned int events)
{
    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

// One of a CGI child's pipes is ready. `fd` is the pipe; pipeOwner tells us
// which client it belongs to. We either feed the child more body (inFd) or
// read its output (outFd); on EOF we reap and build the client's response.
static void handleCgiEvent(int epfd, int fd, unsigned int revents,
                           std::map<int, ClientConnection> &clients,
                           std::map<int, int> &pipeOwner)
{
    int clientFd = pipeOwner[fd];
    std::map<int, ClientConnection>::iterator cit = clients.find(clientFd);
    if (cit == clients.end())
        return;
    ClientConnection &c = cit->second;

    // ---- writing the request body into the child's stdin ----
    if (fd == c.cgiInFd && (revents & (EPOLLOUT | EPOLLHUP | EPOLLERR)))
    {
        int w = write(fd, c.cgiInBuf.c_str() + c.cgiInOff,
                          c.cgiInBuf.size() - c.cgiInOff);
        if (w > 0)
            c.cgiInOff += static_cast<size_t>(w);
        if (w <= 0 || c.cgiInOff >= c.cgiInBuf.size())
        {
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);                 // EOF to the child: "body is complete"
            pipeOwner.erase(fd);
            c.cgiInFd = -1;
        }
        return;
    }

    // ---- reading the child's stdout ----
    if (fd == c.cgiOutFd && (revents & (EPOLLIN | EPOLLHUP | EPOLLERR)))
    {
        char buf[BUF_SIZE];
        int r = read(fd, buf, sizeof(buf));
        if (r > 0)
        {
            c.cgiOutBuf.append(buf, static_cast<size_t>(r));
            return;                    // more may come; stay registered
        }

        // r == 0 (EOF) or r == -1: the script is done producing output.
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        pipeOwner.erase(fd);
        c.cgiOutFd = -1;

        if (c.cgiInFd != -1)           // child died before we finished the body
        {
            epoll_ctl(epfd, EPOLL_CTL_DEL, c.cgiInFd, NULL);
            close(c.cgiInFd);
            pipeOwner.erase(c.cgiInFd);
            c.cgiInFd = -1;
        }

        int status;
        waitpid(c.cgiPid, &status, 0);  // EOF means it has closed stdout → about to exit
        c.cgiPid = -1;

        c.writeBuffer = buildCgiResponse(c.cgiOutBuf);
        c.state = WRITING;
        flipToWrite(epfd, clientFd);    // hand off to your existing send path
    }
}

// Called once per loop iteration: any CGI child running longer than
// CGI_TIMEOUT is killed and its client answered with 504. This is our
// "requests never hang" guarantee for CGI.
static void reapTimedOutCgi(int epfd, std::map<int, ClientConnection> &clients,
                            std::map<int, int> &pipeOwner)
{
    time_t now = time(NULL);
    for (std::map<int, ClientConnection>::iterator it = clients.begin();
         it != clients.end(); ++it)
    {
        ClientConnection &c = it->second;
        if (c.state != CGI_RUNNING || now - c.cgiStart < CGI_TIMEOUT)
            continue;

        if (c.cgiPid > 0)
        {
            kill(c.cgiPid, SIGKILL);        // force the stuck script to die
            waitpid(c.cgiPid, NULL, 0);     // reap it (SIGKILL => it exits promptly)
            c.cgiPid = -1;
        }
        if (c.cgiInFd != -1)
        {
            epoll_ctl(epfd, EPOLL_CTL_DEL, c.cgiInFd, NULL);
            close(c.cgiInFd);
            pipeOwner.erase(c.cgiInFd);
            c.cgiInFd = -1;
        }
        if (c.cgiOutFd != -1)
        {
            epoll_ctl(epfd, EPOLL_CTL_DEL, c.cgiOutFd, NULL);
            close(c.cgiOutFd);
            pipeOwner.erase(c.cgiOutFd);
            c.cgiOutFd = -1;
        }
        c.writeBuffer = errorResponse(504, "Gateway Timeout");
        c.state = WRITING;
        flipToWrite(epfd, it->first);
    }
}

// Close connections that have gone silent (no read/write) for too long — the
// defense against slowloris and abandoned keep-alive sockets. CGI_RUNNING
// connections are skipped; reapTimedOutCgi owns their lifetime.
static void reapIdleClients(int epfd, std::map<int, ClientConnection> &clients)
{
    time_t now = time(NULL);
    std::map<int, ClientConnection>::iterator it = clients.begin();
    while (it != clients.end())
    {
        ClientConnection &c = it->second;
        if (c.state == CGI_RUNNING || now - c.lastActive < CLIENT_TIMEOUT)
        {
            ++it;
            continue;
        }
        int fd = it->first;
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        clients.erase(it++);   // erase-while-iterating: post-increment first
    }
}

// Builders emit a "Connection: close" default; when the socket may be reused,
// upgrade that single header line in place.
static void applyKeepAlive(std::string &resp, bool keepAlive)
{
    if (!keepAlive)
        return;
    const std::string closeHdr = "Connection: close\r\n";
    size_t p = resp.find(closeHdr);
    if (p != std::string::npos)
        resp.replace(p, closeHdr.size(), "Connection: keep-alive\r\n");
}

// HTTP/1.1 defaults to persistent; HTTP/1.0 defaults to close. A Connection
// header overrides the default in either direction.
static bool shouldKeepAlive(const HttpRequest &req)
{
    std::string conn;
    std::map<std::string, std::string>::const_iterator it =
        req.headers.find("connection");
    if (it != req.headers.end())
    {
        conn = it->second;
        for (size_t i = 0; i < conn.size(); ++i)
            conn[i] = std::tolower(static_cast<unsigned char>(conn[i]));
    }

    if (req.version == "HTTP/1.1")
        return conn.find("close") == std::string::npos;      // persistent unless told to close
    return conn.find("keep-alive") != std::string::npos;     // 1.0: only if explicitly asked
}

void runEventLoop(const std::vector<ServerConfig> &servers)
{
    std::map<int, size_t> listenFdToServer;

    for (size_t i = 0; i < servers.size(); ++i)
    {
        int fd = createListeningSocket(servers[i].host, servers[i].port);
        listenFdToServer[fd] = i;
    }

    int epfd = epoll_create(1);
    if (epfd == -1)
        throw std::runtime_error(std::string("epoll_create: ") + strerror(errno));

    for (std::map<int, size_t>::const_iterator it = listenFdToServer.begin();
         it != listenFdToServer.end(); ++it)
    {
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = it->first;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, it->first, &ev) == -1)
            throw std::runtime_error(std::string("epoll_ctl: ") + strerror(errno));
    }

    std::map<int, ClientConnection> clients;
    std::map<int, int> pipeOwner;   // pipe fd -> owning client fd
    struct epoll_event events[MAX_EVENTS];

    while (true)
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
        if (n == -1)
        {
            if (errno == EINTR)
                continue;   // a signal interrupted the wait — just loop again
            throw std::runtime_error(std::string("epoll_wait: ") + strerror(errno));
        }
        for (int i = 0; i < n; ++i)
        {
            try
            {
                int fd = events[i].data.fd;
                std::map<int, size_t>::const_iterator lit = listenFdToServer.find(fd);

                if (lit != listenFdToServer.end())
                {
                    // A listening socket is readable: a new client is waiting.
                    int clientFd = accept(fd, NULL, NULL);
                    if (clientFd == -1)
                        continue;
                    if (fcntl(clientFd, F_SETFL, O_NONBLOCK) == -1)
                    {
                        close(clientFd);
                        continue;
                    }
                    struct epoll_event clientEv;
                    std::memset(&clientEv, 0, sizeof(clientEv));
                    clientEv.events = EPOLLIN;
                    clientEv.data.fd = clientFd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, clientFd, &clientEv) == -1)
                    {
                        close(clientFd);
                        continue;
                    }
                    clients[clientFd].serverIndex = lit->second;
                    clients[clientFd].lastActive = time(NULL);
                }
                else if (pipeOwner.find(fd) != pipeOwner.end())
                {
                    handleCgiEvent(epfd, fd, events[i].events, clients, pipeOwner);
                }
                else
                {
                    std::map<int, ClientConnection>::iterator cit = clients.find(fd);
                    if (cit == clients.end())
                        continue;

                    if (events[i].events & EPOLLIN)
                    {
                        // Client socket is readable: pull bytes into its buffer.
                        char buf[BUF_SIZE];
                        int r = recv(fd, buf, sizeof(buf), 0);
                        if (r <= 0)
                        {
                            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                            clients.erase(cit);
                            continue;
                        }
                        cit->second.buffer.append(buf, r);
                        cit->second.lastActive = time(NULL);
                        // ---- Stage 1: do we have the whole header block yet? ----
                        size_t pos = cit->second.buffer.find("\r\n\r\n");
                        if (pos == std::string::npos)
                            continue;   // headers incomplete, wait for next EPOLLIN

                        const ServerConfig &srv = servers[cit->second.serverIndex];

                        // Parse the header block so we can read Content-Length.
                        HttpRequest req;
                        bool reusable = false;   // true only once we hold a cleanly delimited request
                        if (!parseRequest(cit->second.buffer.substr(0, pos + 2), req))
                        {
                            cit->second.keepAlive = reusable;
                            cit->second.writeBuffer = errorResponse(400, "Bad Request");
                            flipToWrite(epfd, fd);
                            continue;
                        }

                        // ---- Stage 2: get the full body (chunked or Content-Length) ----
                        size_t headerEnd = pos + 4;   // first byte of the body

                        bool chunked = false;
                        std::map<std::string, std::string>::const_iterator te =
                            req.headers.find("transfer-encoding");
                        if (te != req.headers.end() &&
                            te->second.find("chunked") != std::string::npos)
                            chunked = true;

                        if (chunked)
                        {
                            // Un-chunk before anyone (CGI included) sees the body.
                            std::string decoded;
                            ChunkStatus cs =
                                decodeChunked(cit->second.buffer.substr(headerEnd), decoded);
                            if (cs == CHUNK_INCOMPLETE)
                                continue;                 // wait for more bytes
                            if (cs == CHUNK_ERROR)
                            {
                                cit->second.writeBuffer = errorResponse(400, "Bad Request");
                                flipToWrite(epfd, fd);
                                continue;
                            }
                            if (decoded.size() > srv.client_max_body_size)
                            {
                                cit->second.writeBuffer = errorResponse(413, "Payload Too Large");
                                flipToWrite(epfd, fd);
                                continue;
                            }
                            req.body = decoded;
                        }
                        else
                        {
                            size_t contentLength = 0;
                            std::map<std::string, std::string>::const_iterator cl =
                                req.headers.find("content-length");
                            if (cl != req.headers.end())
                            {
                                std::istringstream iss(cl->second);
                                iss >> contentLength;     // TODO: reject non-numeric with 400
                            }

                            // Refuse an oversized body up front, before waiting for it.
                            if (contentLength > srv.client_max_body_size)
                            {
                                cit->second.writeBuffer = errorResponse(413, "Payload Too Large");
                                flipToWrite(epfd, fd);
                                continue;
                            }

                            // Body not fully arrived yet: keep reading on next EPOLLIN.
                            if (cit->second.buffer.size() < headerEnd + contentLength)
                                continue;

                            // TODO(phase10): with keep-alive, erase the consumed request
                            // from buffer instead of relying on Connection: close.
                            req.body = cit->second.buffer.substr(headerEnd, contentLength);
                            reusable = true;
                            cit->second.consumed = headerEnd + contentLength;
                        }

                        std::string interp, script;
                        if (resolveCgi(srv, req, interp, script))
                        {
                            struct stat st;
                            if (stat(script.c_str(), &st) == -1 || S_ISDIR(st.st_mode))
                            {
                                cit->second.writeBuffer = errorResponse(404, "Not Found");
                                flipToWrite(epfd, fd);
                                continue;
                            }
                            CgiProcess proc;
                            if (!startCgi(srv, req, interp, script, proc))
                            {
                                cit->second.writeBuffer = errorResponse(500, "Internal Server Error");
                                flipToWrite(epfd, fd);
                                continue;
                            }

                            cit->second.state    = CGI_RUNNING;
                            cit->second.cgiPid   = proc.pid;
                            cit->second.cgiOutFd = proc.outFd;
                            cit->second.cgiStart = time(NULL);
                            cit->second.cgiOutBuf.clear();

                            epollMod(epfd, fd, 0);                  // client goes idle
                            epollAdd(epfd, proc.outFd, EPOLLIN);    // watch script output
                            pipeOwner[proc.outFd] = fd;

                            if (!req.body.empty())
                            {
                                cit->second.cgiInFd  = proc.inFd;
                                cit->second.cgiInBuf = req.body;
                                cit->second.cgiInOff = 0;
                                epollAdd(epfd, proc.inFd, EPOLLOUT); // feed the body
                                pipeOwner[proc.inFd] = fd;
                            }
                            else
                            {
                                close(proc.inFd);                    // no body -> EOF now
                                cit->second.cgiInFd = -1;
                            }
                            continue;
                        }

                        cit->second.keepAlive   = reusable && shouldKeepAlive(req);
                        cit->second.writeBuffer = handleRequest(srv, req);
                        applyKeepAlive(cit->second.writeBuffer, cit->second.keepAlive);
                        flipToWrite(epfd, fd);
                    }
                    else if (events[i].events & EPOLLOUT)
                    {
                        // Client socket is writable: send as much as it will take.
                        std::string &out = cit->second.writeBuffer;
                        int s = send(fd, out.c_str(), out.size(), 0);
                        if (s > 0)
                        {
                            out.erase(0, s);
                            cit->second.lastActive = time(NULL);
                        }
                        if (s <= 0)
                        {
                            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                            clients.erase(cit);
                        }
                        else if (out.empty())
                        {
                            if (cit->second.keepAlive)
                            {
                                // Reuse: drop the consumed request, wait for the next one.
                                cit->second.buffer.erase(0, cit->second.consumed);
                                cit->second.consumed  = 0;
                                cit->second.keepAlive = false;
                                cit->second.state     = READING;
                                epollMod(epfd, fd, EPOLLIN);
                            }
                            else
                            {
                                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                                close(fd);
                                clients.erase(cit);
                            }
                        }
                    }
                }
            }
            catch (const std::exception &e)
            {
                std::cerr << "webserv: dropped a client event: " << e.what() << std::endl;
            }
        }
        try 
        {
            reapTimedOutCgi(epfd, clients, pipeOwner);
            reapIdleClients(epfd, clients);
        }
        catch (const std::exception &e)
        {
            std::cerr << "webserv: reaper error: " << e.what() << std::endl;
        }
    }
}
