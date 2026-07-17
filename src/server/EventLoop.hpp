#ifndef EVENT_LOOP_HPP
#define EVENT_LOOP_HPP

#include "../config/ConfigTypes.hpp"
#include "../http/HttpRequest.hpp"
#include "../http/RequestHandler.hpp"
#include "Listener.hpp"
#include <sys/epoll.h>
#include <map>
#include <vector>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <iostream>

#define MAX_EVENTS 64
#define BUF_SIZE 4096

struct ClientConnection
{
    size_t      serverIndex;
    std::string buffer;       // inbound: request bytes accumulated across reads
    std::string writeBuffer;  // outbound: response bytes not yet sent
};

void runEventLoop(const std::vector<ServerConfig> &servers);

#endif