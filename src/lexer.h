
#ifndef LEXER_H
#define LEXER_H

#include "general.h"

struct Token {
    enum Type {
        DOT         = '.',
        EQUALS      = '=',
        SEMICOLON   = ';',
        COLON       = ':',
        LEFT_PAREN  = '(',
        RIGHT_PAREN = ')',
        STAR        = '*',
        SLASH       = '/',
        PERCENT     = '%',
        PLUS        = '+',
        MINUS       = '-',

        END = 256,
        INTEGER,
        IDENTIFIER,
        STRING,


        KEYWORD_FUNC,
        KEYWORD_VAR,
        KEYWORD_VOID,
        KEYWORD_STRING,
        KEYWORD_INT,
        KEYWORD_UINT,
        KEYWORD_UINT8,
        KEYWORD_UINT16,
        KEYWORD_UINT32,
        KEYWORD_UINT64,
        KEYWORD_INT8,
        KEYWORD_INT16,
        KEYWORD_INT32,
        KEYWORD_INT64,
        KEYWORD_FLOAT,
        KEYWORD_DOUBLE,

        ARROW,              // ->
        DEREFERENCE_OR_SHIFT, // <<

        COMMENT,
    };

    Type type;
    TextSpan text_span;
    String filename;

    String string;
    s64 integer;

    Token() {}

    Token(Type type, TextSpan span) {
        this->type = type;
        this->text_span = span;
    }
};

struct Compiler;

struct Lexer {
    String filename;

    string_length_type current_char = 0;
    String text;

    Array<Token> tokens;
    Compiler *compiler;

    Lexer(Compiler *compiler, String input_text, String filename) {
        this->text = input_text;
        this->filename = filename;
        this->compiler = compiler;
    }

    Token make_token(Token::Type type, Span span);
    Token make_eof_token();
    Token make_string_token(Token::Type type, Span span, String string);
    Token make_integer_token(s64 value, Span span);
    
    void eat_whitespace();
    Token lex_token();
    void tokenize_text();
};

#endif
