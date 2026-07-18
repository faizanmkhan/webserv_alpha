#ifndef CGI_HANDLER_HPP
#define CGI_HANDLER_HPP

#include "../config/ConfigTypes.hpp"
#include "HttpRequest.hpp"
#include "Router.hpp"
#include "RequestHandler.hpp"
#include <sys/types.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <cstring>

struct CgiProcess
{
    pid_t pid;    // child pid, for waitpid/kill
    int   inFd;   // WE write the request body here (child's stdin)
    int   outFd;  // WE read the script's output here (child's stdout)
};

// Fork a CGI child for `scriptPath` run by `interpreter`. On success returns
// true and fills `proc` with the child's pid + the parent's pipe ends
// (already set non-blocking). Returns false if pipe()/fork() failed.
bool startCgi(const ServerConfig &srv, const HttpRequest &req,
              const std::string &interpreter, const std::string &scriptPath,
              CgiProcess &proc);

// If req maps to a CGI script under a matched location, fill interpreter +
// filesystem path and return true; otherwise false (serve it normally).
bool resolveCgi(const ServerConfig &srv, const HttpRequest &req,
                std::string &interpreter, std::string &scriptPath);

// Turn raw CGI stdout (its own headers + blank line + body) into a full HTTP
// response, supplying Content-Length ourselves and honoring a Status: header.
std::string buildCgiResponse(const std::string &raw);

#endif