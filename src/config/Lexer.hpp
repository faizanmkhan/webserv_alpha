#ifndef LEXER_HPP
#define LEXER_HPP

#include <string>
#include <vector>

enum TokenType
{
    TOKEN_WORD,        // any bare word: directive names, values, paths, numbers, host:port...
    TOKEN_OPEN_BRACE,  // {
    TOKEN_CLOSE_BRACE, // }
    TOKEN_SEMICOLON    // ;
};

struct Token
{
    TokenType   type;
    std::string value; // the text itself for TOKEN_WORD; empty for the punctuation tokens
};

std::vector<Token> tokenize(const std::string &content);

#endif
