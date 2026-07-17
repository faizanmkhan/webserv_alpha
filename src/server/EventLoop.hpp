#ifndef EVENT_LOOP_HPP
#define EVENT_LOOP_HPP

#include "../config/ConfigTypes.hpp"
#include "Listener.hpp"
#include <sys/epoll.h>
#include <map>
#include <vector>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <stdexcept>

#define MAX_EVENTS 64
#define BUF_SIZE 4096

void runEventLoop(const std::vector<ServerConfig> &servers);

#endif