#include "Lexer.hpp"

std::vector<Token> tokenize(const std::string &content) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < content.size())
    {
        char c = content[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            i++;
        else if (c == '#')
        {
            while (i < content.size() && content[i] != '\n')
                i++;
        }
        else if (c == '{')
        {
            Token t = {TOKEN_OPEN_BRACE, ""};
            tokens.push_back(t);
            i++;
        }
        else if (c == '}')
        {
            Token t = {TOKEN_CLOSE_BRACE, ""};
            tokens.push_back(t);
            i++;
        }
        else if (c == ';')
        {
            Token t = {TOKEN_SEMICOLON, ""};
            tokens.push_back(t);
            i++;
        }
        else
        {
            size_t start = i;
            while (i < content.size() && content[i] != ' ' && content[i] != '\t'
                   && content[i] != '\n' && content[i] != '\r' && content[i] != '#'
                   && content[i] != '{' && content[i] != '}' && content[i] != ';')
                   i++;
                Token t = {TOKEN_WORD, content.substr(start, i - start)};
                tokens.push_back(t);
        }
    }
    return tokens;
}