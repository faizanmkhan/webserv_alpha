#include "config/Lexer.hpp"
#include "config/ConfigParser.hpp"
#include "server/EventLoop.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>

static void printLocation(const LocationConfig &loc)
{
    std::cout << "  location " << loc.path << " {" << std::endl;
    std::cout << "    methods:";
    for (size_t i = 0; i < loc.methods.size(); ++i)
        std::cout << " " << loc.methods[i];
    std::cout << std::endl;
    std::cout << "    root: " << loc.root << std::endl;
    std::cout << "    index: " << loc.index << std::endl;
    std::cout << "    autoindex: " << (loc.autoindex ? "on" : "off") << std::endl;
    std::cout << "    upload_store: " << loc.upload_store << std::endl;
    for (std::map<std::string, std::string>::const_iterator it = loc.cgi_ext.begin(); it != loc.cgi_ext.end(); ++it)
        std::cout << "    cgi_ext: " << it->first << " -> " << it->second << std::endl;
    if (loc.has_redirect)
        std::cout << "    redirect: " << loc.redirect_code << " " << loc.redirect_target << std::endl;
    std::cout << "  }" << std::endl;
}

static void printServer(const ServerConfig &srv)
{
    std::cout << "server {" << std::endl;
    std::cout << "  listen: " << srv.host << ":" << srv.port << std::endl;
    std::cout << "  server_name: " << srv.server_name << std::endl;
    std::cout << "  client_max_body_size: " << srv.client_max_body_size << std::endl;
    for (std::map<int, std::string>::const_iterator it = srv.error_pages.begin(); it != srv.error_pages.end(); ++it)
        std::cout << "  error_page: " << it->first << " -> " << it->second << std::endl;
    for (size_t i = 0; i < srv.locations.size(); ++i)
        printLocation(srv.locations[i]);
    std::cout << "}" << std::endl;
}

int main(int argc, char **argv)
{
    std::string path = (argc > 1) ? argv[1] : "config/default.conf";
    std::ifstream file(path.c_str());
    if (!file)
    {
        std::cerr << "webserv: cannot open config file: " << path << std::endl;
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    std::vector<Token> tokens = tokenize(content);

    try
    {
        ConfigParser parser(tokens);
        std::vector<ServerConfig> servers = parser.parse();
        for (size_t i = 0; i < servers.size(); ++i)
            printServer(servers[i]);
        runEventLoop(servers);
    }
    catch (const std::exception &e)
    {
        std::cerr << "webserv: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
