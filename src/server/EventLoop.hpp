#ifndef EVENT_LOOP_HPP
#define EVENT_LOOP_HPP
#include "../http/CgiHandler.hpp"
#include "../config/ConfigTypes.hpp"
#include "../http/HttpRequest.hpp"
#include "../http/RequestHandler.hpp"
#include "Listener.hpp"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <ctime>
#include <map>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <iostream>
#include <ctime>

#define MAX_EVENTS 64
#define BUF_SIZE 4096

enum ConnState { 
    READING, 
    CGI_RUNNING, 
    WRITING };

struct ClientConnection
{
    size_t      serverIndex;
    std::string buffer;       // inbound: request bytes accumulated across reads
    std::string writeBuffer;  // outbound: response bytes not yet sent

    // --- CGI job state (only meaningful while state == CGI_RUNNING) ---
    ConnState   state;          // where this connection is in its lifecycle
    pid_t       cgiPid;         // child process id (-1 = none)
    int         cgiInFd;        // pipe A write-end: WE write the request body here
    int         cgiOutFd;       // pipe B read-end:  WE read the script's output here
    std::string cgiInBuf;       // body still to be written to the child
    size_t      cgiInOff;       // how much of cgiInBuf we've already written
    std::string cgiOutBuf;      // script output accumulated so far
    time_t      cgiStart;       // fork timestamp, for the 504 timeout

    ClientConnection()
        : serverIndex(0), state(READING), cgiPid(-1),
          cgiInFd(-1), cgiOutFd(-1), cgiInOff(0), cgiStart(0) {}
};

void runEventLoop(const std::vector<ServerConfig> &servers);

#endif