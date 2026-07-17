#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include "Lexer.hpp"
#include "ConfigTypes.hpp"
#include <vector>
#include <string>

class ConfigParser
{
    public:
        ConfigParser(const std::vector<Token> &tokens);

        std::vector<ServerConfig> parse();

    private:
        const std::vector<Token> &_tokens;
        size_t                    _pos;

        bool         atEnd() const;
        const Token &current() const;
        const Token &advance();          // returns the token before advancing
        bool         checkWord(const std::string &word) const;
        void         expect(TokenType type, const std::string &context);

        ServerConfig   parseServer();
        LocationConfig parseLocation();
        void           parseServerDirective(ServerConfig &srv);
        void           parseLocationDirective(LocationConfig &loc);
};

#endif
