#ifndef LISTENER_HPP
#define LISTENER_HPP

#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <stdexcept>

int createListeningSocket(const std::string &host, int port);

#endif