#include "EventLoop.hpp"
#include <sstream>

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
    struct epoll_event events[MAX_EVENTS];

    while (true)
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1)
            throw std::runtime_error(std::string("epoll_wait: ") + strerror(errno));

        for (int i = 0; i < n; ++i)
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

                    // ---- Stage 1: do we have the whole header block yet? ----
                    size_t pos = cit->second.buffer.find("\r\n\r\n");
                    if (pos == std::string::npos)
                        continue;   // headers incomplete, wait for next EPOLLIN

                    const ServerConfig &srv = servers[cit->second.serverIndex];

                    // Parse the header block so we can read Content-Length.
                    HttpRequest req;
                    if (!parseRequest(cit->second.buffer.substr(0, pos + 2), req))
                    {
                        cit->second.writeBuffer = errorResponse(400, "Bad Request");
                        flipToWrite(epfd, fd);
                        continue;
                    }

                    // ---- Stage 2: how long is the body, and is it all here? ----
                    size_t headerEnd = pos + 4;   // first byte of the body
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

                    // Body not fully arrived yet: keep reading on the next EPOLLIN.
                    if (cit->second.buffer.size() < headerEnd + contentLength)
                        continue;

                    // Full request in hand: slice out the body and dispatch.
                    // TODO(phase10): with keep-alive we must erase the consumed
                    // request from buffer instead of relying on Connection: close.
                    req.body = cit->second.buffer.substr(headerEnd, contentLength);
                    cit->second.writeBuffer = handleRequest(srv, req);
                    flipToWrite(epfd, fd);
                }
                else if (events[i].events & EPOLLOUT)
                {
                    // Client socket is writable: send as much as it will take.
                    std::string &out = cit->second.writeBuffer;
                    int s = send(fd, out.c_str(), out.size(), 0);
                    if (s > 0)
                        out.erase(0, s);
                    if (s <= 0 || out.empty())
                    {
                        // Sent it all (or the send failed): close the connection.
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        clients.erase(cit);
                    }
                    // else: partial send, keep the rest for the next EPOLLOUT
                }
            }
        }
    }
}
