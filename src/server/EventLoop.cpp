#include "EventLoop.hpp"

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

                    size_t pos = cit->second.buffer.find("\r\n\r\n");
                    if (pos != std::string::npos)
                    {
                        // Full header block received: build the response now,
                        // then flip this fd to writing and send it on EPOLLOUT.
                        HttpRequest req;
                        const ServerConfig &srv = servers[cit->second.serverIndex];
                        if (parseRequest(cit->second.buffer.substr(0, pos + 2), req))
                            cit->second.writeBuffer = handleRequest(srv, req);
                        else
                            cit->second.writeBuffer = errorResponse(400, "Bad Request");

                        struct epoll_event wev;
                        std::memset(&wev, 0, sizeof(wev));
                        wev.events = EPOLLOUT;
                        wev.data.fd = fd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &wev);
                    }
                    // else: headers incomplete, keep reading on the next EPOLLIN
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
