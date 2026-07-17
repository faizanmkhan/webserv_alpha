#include "Listener.hpp"

int createListeningSocket(const std::string &host, int port) 
{
    std::ostringstream oss;
    oss << port;
    std::string portStr = oss.str();

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res;
    int status = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (status != 0)
        throw std::runtime_error("getaddrinfo: " + std::string(gai_strerror(status)));

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == -1)
    {
        freeaddrinfo(res);
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    }
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        freeaddrinfo(res);
        close(fd);
        throw std::runtime_error(std::string("setsockopt: ") + strerror(errno));
    }
    if (bind(fd, res->ai_addr, res->ai_addrlen) == -1)
    {
        freeaddrinfo(res);
        close(fd);
        throw std::runtime_error(std::string("bind: ") + strerror(errno));
    }
    freeaddrinfo(res);
    if (listen(fd, SOMAXCONN) == -1)
    {
        close(fd);
        throw std::runtime_error(std::string("listen: ") + strerror(errno));
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1)
    {
        close(fd);
        throw std::runtime_error(std::string("fcntl: ") + strerror(errno));
    }
    return fd;
}