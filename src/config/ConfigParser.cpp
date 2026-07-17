#include "ConfigParser.hpp"
#include <stdexcept>
#include <cstdlib>

ConfigParser::ConfigParser(const std::vector<Token> &tokens) : _tokens(tokens), _pos(0)
{
}

bool ConfigParser::atEnd() const
{
    return _pos >= _tokens.size();
}

const Token &ConfigParser::current() const
{
    if (atEnd())
        throw std::runtime_error("unexpected end of config file");
    return _tokens[_pos];
}

const Token &ConfigParser::advance()
{
    const Token &tok = current();
    _pos++;
    return tok;
}

bool ConfigParser::checkWord(const std::string &word) const
{
    return !atEnd() && current().type == TOKEN_WORD && current().value == word;
}

void ConfigParser::expect(TokenType type, const std::string &context)
{
    if (atEnd() || current().type != type)
        throw std::runtime_error("config error near '" + context + "': unexpected token");
    advance();
}

std::vector<ServerConfig> ConfigParser::parse()
{
    std::vector<ServerConfig> servers;

    while (!atEnd())
        servers.push_back(parseServer());
    return servers;
}

ServerConfig ConfigParser::parseServer()
{
    ServerConfig srv;

    if (!checkWord("server"))
        throw std::runtime_error("expected 'server' block at top level");
    advance();
    expect(TOKEN_OPEN_BRACE, "server");
    while (!atEnd() && current().type != TOKEN_CLOSE_BRACE)
    {
        if (checkWord("location"))
            srv.locations.push_back(parseLocation());
        else
            parseServerDirective(srv);
    }
    expect(TOKEN_CLOSE_BRACE, "server");
    return srv;
}

LocationConfig ConfigParser::parseLocation()
{
    LocationConfig loc;

    advance(); // consume "location"
    if (atEnd() || current().type != TOKEN_WORD)
        throw std::runtime_error("expected a path after 'location'");
    loc.path = advance().value;
    expect(TOKEN_OPEN_BRACE, "location");
    while (!atEnd() && current().type != TOKEN_CLOSE_BRACE)
        parseLocationDirective(loc);
    expect(TOKEN_CLOSE_BRACE, "location");
    return loc;
}

void ConfigParser::parseServerDirective(ServerConfig &srv)
{
    if (checkWord("listen"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected host:port after 'listen'");
        std::string value = advance().value;
        size_t colon = value.find(':');
        if (colon == std::string::npos)
        {
            srv.host = "0.0.0.0";
            srv.port = std::atoi(value.c_str());
        }
        else
        {
            srv.host = value.substr(0, colon);
            srv.port = std::atoi(value.substr(colon + 1).c_str());
        }
        expect(TOKEN_SEMICOLON, "listen");
    }
    else if (checkWord("server_name"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a name after 'server_name'");
        srv.server_name = advance().value;
        expect(TOKEN_SEMICOLON, "server_name");
    }
    else if (checkWord("error_page"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a status code after 'error_page'");
        int code = std::atoi(advance().value.c_str());
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a path after error_page code");
        std::string errPath = advance().value;
        srv.error_pages[code] = errPath;
        expect(TOKEN_SEMICOLON, "error_page");
    }
    else if (checkWord("client_max_body_size"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a size after 'client_max_body_size'");
        std::string value = advance().value;
        char unit = value[value.size() - 1];
        size_t multiplier = 1;
        if (unit == 'K' || unit == 'k') multiplier = 1024;
        else if (unit == 'M' || unit == 'm') multiplier = 1024 * 1024;
        else if (unit == 'G' || unit == 'g') multiplier = 1024 * 1024 * 1024;
        std::string numberPart = (multiplier > 1) ? value.substr(0, value.size() - 1) : value;
        srv.client_max_body_size = std::atol(numberPart.c_str()) * multiplier;
        expect(TOKEN_SEMICOLON, "client_max_body_size");
    }
    else
        throw std::runtime_error("unknown directive: " + current().value);
}

void ConfigParser::parseLocationDirective(LocationConfig &loc)
{
    if (checkWord("methods"))
    {
        advance();
        while (!atEnd() && current().type == TOKEN_WORD)
            loc.methods.push_back(advance().value);
        expect(TOKEN_SEMICOLON, "methods");
    }
    else if (checkWord("root"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a path after 'root'");
        loc.root = advance().value;
        expect(TOKEN_SEMICOLON, "root");
    }
    else if (checkWord("index"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a filename after 'index'");
        loc.index = advance().value;
        expect(TOKEN_SEMICOLON, "index");
    }
    else if (checkWord("autoindex"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected 'on' or 'off' after 'autoindex'");
        loc.autoindex = (advance().value == "on");
        expect(TOKEN_SEMICOLON, "autoindex");
    }
    else if (checkWord("upload_store"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a path after 'upload_store'");
        loc.upload_store = advance().value;
        expect(TOKEN_SEMICOLON, "upload_store");
    }
    else if (checkWord("cgi_ext"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected an extension after 'cgi_ext'");
        std::string ext = advance().value;
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected an interpreter path after cgi_ext extension");
        std::string interpreter = advance().value;
        loc.cgi_ext[ext] = interpreter;
        expect(TOKEN_SEMICOLON, "cgi_ext");
    }
    else if (checkWord("return"))
    {
        advance();
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a status code after 'return'");
        loc.redirect_code = std::atoi(advance().value.c_str());
        if (atEnd() || current().type != TOKEN_WORD)
            throw std::runtime_error("expected a target after return code");
        loc.redirect_target = advance().value;
        loc.has_redirect = true;
        expect(TOKEN_SEMICOLON, "return");
    }
    else
        throw std::runtime_error("unknown directive: " + current().value);
}
