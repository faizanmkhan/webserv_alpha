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

    struct epoll_event events[MAX_EVENTS];

    while (true)
    {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1)
            throw std::runtime_error(std::string("epoll_wait: ") + strerror(errno));

        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;

            if (listenFdToServer.count(fd))
            {
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
                    close(clientFd);
            }
            else
            {
                char buf[BUF_SIZE];
                int r = recv(fd, buf, sizeof(buf), 0);
                if (r > 0)
                {
                    static const char response[] =
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 13\r\n\r\nHello, world!";
                    send(fd, response, sizeof(response) - 1, 0);
                }
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
            }
        }
    }
}