// Griffin port of the line-numbered BASIC interpreter (../../../basic/basic.cpp).
//
// The interpreter itself is unchanged: it is an ordinary POSIX/C++ program and
// runs on the firmware's newlib syscalls plus the apps/lib POSIX veneer
// (gettimeofday/usleep/poll/opendir).  The port adds only BREAK (poll the
// console between program lines), the FILES/TIMER/DATE$/TIME$ dialect words,
// and a seed for RND.
//
// -std=c++23 defines __STRICT_ANSI__, which hides newlib's non-ISO
// declarations -- including drand48()/srand48(), which the interpreter has
// always used for RND.  Asking for the default (BSD-visible) namespace brings
// them back; it must precede every #include.
#define _DEFAULT_SOURCE

#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <set>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <variant>
#include <unordered_map>
#include <map>
#include <new>
#include <cassert>

// The only Griffin-specific interface used here: griffin_getticks() seeds RND,
// and griffin_readdir() backs FILES (the POSIX readdir() veneer cannot report
// file sizes, and stat() is a firmware stub).
#include "../lib/griffin_app.h"

/*
all statements should throw parse errors on !(COLON | end)
Use C++ exception classes and construct the string?
Unify console output so printing an error moves the column
use Variant with visit lambda with if-else chain
*/

const bool debug_state = false;
const bool debug_statements = false;

enum TokenType
{
    TEST, // XXX for bringup
    STRING_IDENTIFIER,
    NUMBER_IDENTIFIER,
    DOUBLE,
    INTEGER,
    STRING,
    REMARK,  		 // Remark (comment) starting with REM keyword
    ABS,
    ATN,
    COS,
    EXP,
    INT,
    LOG,
    RND,
    SGN,
    SIN,
    SQR,
    TAN,
    LEFT,
    RIGHT,
    MID,
    LEN,
    STR,
    TAB,
    VAL,
    CHR,
    TIMER,
    DATE,
    TIME,
    NOT_EQUAL,
    LESS_THAN,
    GREATER_THAN,
    LESS_THAN_EQUAL,
    GREATER_THAN_EQUAL,
    OPEN_PAREN,
    CLOSE_PAREN,
    EQUAL,
    PLUS,
    MINUS,
    MULTIPLY,
    DIVIDE,
    AND,
    OR,
    NOT,
    POWER,
    COMMA,
    COLON,
    SEMICOLON,
    WAIT,
    DEF,
    FN,
    DIM,
    LET,
    IF,
    THEN,
    ELSE,
    FOR,
    TO,
    STEP,
    NEXT,
    GOTO,
    GOSUB,
    RETURN,
    PRINT,
    INPUT,
    END,
    WIDTH,
    CLEAR,
    RUN,
    STOP,
    ON,
    READ,
    ORDER,
    DATA,
    TOKENTYPE_END,
};


std::unordered_map<TokenType, int> operator_precedence = {
    { ABS, -1000 },
    { ATN, -1000 },
    { COS, -1000 },
    { EXP, -1000 },
    { INT, -1000 },
    { LOG, -1000 },
    { RND, -1000 },
    { SGN, -1000 },
    { SIN, -1000 },
    { SQR, -1000 },
    { TAN, -1000 },
    { LEFT, -1000 },
    { RIGHT, -1000 },
    { MID, -1000 },
    { LEN, -1000 },
    { STR, -1000 },
    { TAB, -1000 },
    { VAL, -1000 },
    { CHR, -1000 },
    { TIMER, -1000 },
    { DATE, -1000 },
    { TIME, -1000 },
    { POWER, 6 },
    { MULTIPLY, 5 }, 
    { DIVIDE, 5 }, 
    { PLUS, 4 }, 
    { MINUS, 4 }, 
    { LESS_THAN, 3 },
    { GREATER_THAN, 3 },
    { LESS_THAN_EQUAL, 3 },
    { GREATER_THAN_EQUAL, 3 },
    { EQUAL, 3 },
    { NOT_EQUAL, 3 },
    { AND, 2 },
    { OR, 2 },
};

std::set<TokenType> function_tokens = {
    ABS,
    ATN,
    COS,
    EXP,
    INT,
    LOG,
    RND,
    SGN,
    SIN,
    SQR,
    TAN,
    LEFT,
    RIGHT,
    MID,
    LEN,
    STR,
    TAB,
    VAL,
    CHR,
    TIMER,
    DATE,
    TIME,
};

std::set<TokenType> operator_tokens = {
    POWER, 
    MULTIPLY, 
    DIVIDE, 
    PLUS, 
    MINUS, 
    LESS_THAN, 
    GREATER_THAN, 
    LESS_THAN_EQUAL, 
    GREATER_THAN_EQUAL, 
    EQUAL, 
    NOT_EQUAL, 
    AND, 
    OR, 
    NOT, 
};

std::set<TokenType> binary_operators = {
    POWER, 
    MULTIPLY, 
    DIVIDE, 
    PLUS, 
    MINUS, 
    LESS_THAN, 
    GREATER_THAN, 
    LESS_THAN_EQUAL, 
    GREATER_THAN_EQUAL, 
    EQUAL, 
    NOT_EQUAL, 
    AND, 
    OR, 
};

std::set<TokenType> unary_operators = {
    PLUS, 
    MINUS, 
    NOT,
};

std::set<TokenType> commands = {
    WAIT,
    DEF,
    FN,
    DIM,
    LET,
    IF,
    THEN,
    ELSE,
    FOR,
    TO,
    STEP,
    NEXT,
    GOTO,
    GOSUB,
    RETURN,
    PRINT,
    INPUT,
    END,
    WIDTH,
    CLEAR,
    RUN,
    STOP,
    ON,
    READ,
    ORDER,
    DATA,
};


std::unordered_map<std::string, TokenType> StringToToken =
{
    {"TEST", TEST},
    {"<>", NOT_EQUAL},
    {"<=", LESS_THAN_EQUAL},
    {">=", GREATER_THAN_EQUAL},
    {">", GREATER_THAN},
    {"<", LESS_THAN},
    {"(", OPEN_PAREN},
    {")", CLOSE_PAREN},
    {"=", EQUAL},
    {"+", PLUS},
    {"-", MINUS},
    {"*", MULTIPLY},
    {"/", DIVIDE},
    {"^", POWER},
    {",", COMMA},
    {":", COLON},
    {";", SEMICOLON},
    {"DIM", DIM},
    {"LET", LET},
    {"IF", IF},
    {"THEN", THEN},
    {"ELSE", ELSE},
    {"FOR", FOR},
    {"TO", TO},
    {"STEP", STEP},
    {"NEXT", NEXT},
    {"GOTO", GOTO},
    {"GOSUB", GOSUB},
    {"RETURN", RETURN},
    {"PRINT", PRINT},
    {"INPUT", INPUT},
    {"END", END},
    {"ABS", ABS},
    {"ATN", ATN},
    {"COS", COS},
    {"EXP", EXP},
    {"INT", INT},
    {"LOG", LOG},
    {"RND", RND},
    {"SGN", SGN},
    {"SIN", SIN},
    {"SQR", SQR},
    {"TAN", TAN},
    {"AND", AND},
    {"OR", OR},
    {"NOT", NOT},
    {"LEFT$", LEFT},
    {"RIGHT$", RIGHT},
    {"MID$", MID},
    {"STR$", STR},
    {"LEN", LEN},
    {"TAB", TAB},
    {"VAL", VAL},
    {"WAIT", WAIT},
    {"DEF", DEF},
    {"FN", FN},
    {"CHR$", CHR},
    {"TIMER", TIMER},
    {"DATE$", DATE},
    {"TIME$", TIME},
    {"WIDTH", WIDTH},
    {"CLEAR", CLEAR},
    {"ON", ON},
    {"READ", READ},
    {"ORDER", ORDER},
    {"DATA", DATA},
    {"RUN", RUN},
    {"STOP", STOP},
};

// The keywords and operators of StringToToken, bucketed by first character and
// sorted longest-first within a bucket, so tokenizing probes only the handful of
// entries that could match at a given position instead of walking the whole map
// at every character.  Matching semantics are unchanged: case-insensitive,
// longest match wins, and there is deliberately no word-boundary check (FORM is
// still FOR followed by the identifier M).
struct KeywordIndex
{
    std::array<std::vector<std::pair<std::string, TokenType>>, 256> by_first_char;

    KeywordIndex()
    {
        // Keys are uppercase, so a bucket is found with toupper() of the source
        // character.
        for(const auto& [word, token]: StringToToken) {
            by_first_char[(unsigned char)std::toupper((unsigned char)word[0])].push_back({word, token});
        }
        for(auto& bucket: by_first_char) {
            std::sort(bucket.begin(), bucket.end(),
                [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
        }
    }
};

const KeywordIndex TokenIndex;

// True if the (uppercase) keyword `word` matches line[index...] case-insensitively.
// A word running off the end of the line does not match.
bool MatchesAt(const std::string& line, size_t index, const std::string& word)
{
    if(index + word.size() > line.size()) {
        return false;
    }
    for(size_t i = 0; i < word.size(); i++) {
        if(std::toupper((unsigned char)line[index + i]) != (unsigned char)word[i]) {
            return false;
        }
    }
    return true;
}

std::unordered_map<TokenType, const char *> TokenTypeToStringMap =
{
    {TEST, "TEST"},
    {EQUAL, "="},
    {NOT_EQUAL, "<>"},
    {LESS_THAN_EQUAL, "<="},
    {GREATER_THAN_EQUAL, ">="},
    {LESS_THAN, "<"},
    {GREATER_THAN, ">"},
    {OPEN_PAREN, "("},
    {CLOSE_PAREN, ")"},
    {PLUS, "+"},
    {MINUS, "-"},
    {MULTIPLY, "*"},
    {DIVIDE, "/"},
    {POWER, "^"},
    {COMMA, ","},
    {COLON, ":"},
    {SEMICOLON, ";"},
    {DIM, "DIM"},
    {LET, "LET"},
    {IF, "IF"},
    {THEN, "THEN"},
    {ELSE, "ELSE"},
    {FOR, "FOR"},
    {TO, "TO"},
    {STEP, "STEP"},
    {NEXT, "NEXT"},
    {GOTO, "GOTO"},
    {GOSUB, "GOSUB"},
    {RETURN, "RETURN"},
    {PRINT, "PRINT"},
    {INPUT, "INPUT"},
    {END, "END"},
    {ABS, "ABS"},
    {ATN, "ATN"},
    {COS, "COS"},
    {EXP, "EXP"},
    {INT, "INT"},
    {LOG, "LOG"},
    {RND, "RND"},
    {SGN, "SGN"},
    {SIN, "SIN"},
    {SQR, "SQR"},
    {TAN, "TAN"},
    {AND, "AND"},
    {OR, "OR"},
    {NOT, "NOT"},
    {LEFT, "LEFT$"},
    {RIGHT, "RIGHT$"},
    {MID, "MID$"},
    {STR, "STR$"},
    {LEN, "LEN"},
    {TAB, "TAB"},
    {VAL, "VAL"},
    {WAIT, "WAIT"},
    {DEF, "DEF"},
    {FN, "FN"},
    {CHR, "CHR$"},
    {TIMER, "TIMER"},
    {DATE, "DATE$"},
    {TIME, "TIME$"},
    {WIDTH, "WIDTH"},
    {CLEAR, "CLEAR"},
    {ON, "ON"},
    {READ, "READ"},
    {ORDER, "ORDER"},
    {DATA, "DATA"},
    {RUN, "RUN"},
    {STOP, "STOP"},
};

struct TokenizeError
{
    enum Type
    {
        SYNTAX
    } type;
    int position;
    TokenizeError(Type type, int position) :
        type(type),
        position(position)
    {}
};

std::string str_toupper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::toupper(c); }
                  );
    return s;
}

struct VariableReference
{
    std::string name;
    std::vector<int32_t> indices;
    VariableReference(const std::string& name, const std::vector<int32_t>& indices) :
        name(name),
        indices(indices)
    {}
};

typedef std::variant<std::string, double, VariableReference> Value;
double to_basic_bool(bool b) { return b ? -1 : 0; }

struct Token
{
    public:
        TokenType type;
        Value value{0.0};

        Token(TokenType type) : type(type) {}
        Token(TokenType type, const std::string& value) : type(type), value(value) {}
        Token(int32_t value) : type(INTEGER), value(static_cast<double>(value)) {}
        Token(double value) : type(DOUBLE), value(value) {}
        operator TokenType() {return type; }
        operator Value() {return value; }
};

std::string str(const Value& v) { return std::get<std::string>(v); }
double num(const Value& v) { return std::get<double>(v); }
int32_t igr(const Value& v) { return static_cast<int32_t>(std::get<double>(v)); }
VariableReference vref(const Value& v) { return std::get<VariableReference>(v); }
bool is_vref(const Value& v) { return std::holds_alternative<VariableReference>(v); }
bool is_str(const Value& v) { return std::holds_alternative<std::string>(v); }
bool is_num(const Value& v) { return std::holds_alternative<double>(v); }
int32_t is_igr(double v) { return v == static_cast<int32_t>(v); }


typedef std::vector<Token> TokenList;
typedef TokenList::const_iterator TokenIterator;

TokenList Tokenize(const std::string& line)
{
    TokenList tokens;
    std::string pending;
    size_t pending_started = 0;

    auto add_pending = [&](int index, char c) {
        if(pending.empty()) {
            pending_started = index;
        }
        pending += c;
    };

    // Classify the pending run of characters as an integer, a float, or an
    // identifier.  strtol/strtod are used instead of stoi/stod because the
    // exceptions those threw for every identifier dominated tokenizing time;
    // the out-of-range exceptions they threw are still thrown here so that
    // behavior is unchanged.  `pending` never holds leading whitespace or a
    // leading sign (both are consumed by the loop below), so only a leading
    // digit or '.' can begin a number -- except that strtod also accepts "INF"
    // and "NAN", which it always has, so those first letters are offered too.
    auto flush_pending = [&]() {
        while (!pending.empty())
        {
            std::size_t ipos{}, dpos{};
            bool found_integer = false, found_float = false;
            int32_t i = 0;
            double d = 0.0;
            const char *begin = pending.c_str();
            char first = pending[0];

            if(isdigit((unsigned char)first) || first == '.' ||
               first == 'I' || first == 'i' || first == 'N' || first == 'n') {
                char *end;

                errno = 0;
                long l = std::strtol(begin, &end, 10);
                if(errno == ERANGE || l < INT_MIN || l > INT_MAX) {
                    throw std::out_of_range("stoi: out of range");
                }
                if(end != begin) {
                    i = static_cast<int32_t>(l);
                    ipos = end - begin;
                    found_integer = true;
                }

                errno = 0;
                d = std::strtod(begin, &end);
                if(errno == ERANGE) {
                    throw std::out_of_range("stod: out of range");
                }
                if(end != begin) {
                    dpos = end - begin;
                    found_float = true;
                }
            }

            if(found_integer && (!found_float || dpos <= ipos)) {
                tokens.push_back(Token(i));
                pending = pending.substr(ipos);
                pending_started += ipos;
            } else if(found_float) {
                tokens.push_back(Token(d));
                pending = pending.substr(dpos);
                pending_started += dpos;
            } else {
                for(size_t k = 0; k < pending.size() - 1; k++) {
                    char c = pending[k];
                    if(!isalnum(c) && c != '_') {
                        throw TokenizeError(TokenizeError::SYNTAX, pending_started + k);
                    }
                }
                char c = pending[pending.size() - 1];
                if(!isalnum(c) && c != '_' && c != '$') {
                    throw TokenizeError(TokenizeError::SYNTAX, pending_started + pending.size() - 1);
                }
                if(pending[pending.size() - 1] == '$') {
                    tokens.push_back(Token(STRING_IDENTIFIER, str_toupper(pending)));
                } else {
                    tokens.push_back(Token(NUMBER_IDENTIFIER, str_toupper(pending)));
                }
                pending.clear();
                pending_started = std::string::npos;
            }
        }
    };

    for (size_t index = 0; index < line.size();) {
        // REM starts a remark, but only at a word boundary so identifiers that
        // happen to contain "REM" (e.g. FOREMAN) are not mistaken for comments.
        // The 'R' test comes first so most characters cost only one compare.
        if(std::toupper((unsigned char)line[index]) == 'R') {
            bool at_word_start = (index == 0) ||
                (!isalnum((unsigned char)line[index - 1]) && line[index - 1] != '_');
            if(at_word_start && MatchesAt(line, index, "REM")) {
                flush_pending();
                tokens.push_back(Token(REMARK, line.substr(index + 3)));
                break;
            }
        }

        char c = line[index];

        if (isspace(c))
        {
            index++;
            continue;
        }

        if(c == '"') {
            flush_pending();
            index++;
            std::string str;
            while(index < line.size() && line[index] != '"') {
                str += line[index];
                index++;
            }
            if(index < line.size()) {
                index++;
            }
            tokens.push_back(Token(STRING, str));
            continue;
        }

        auto result = [&]() -> std::optional<std::pair<size_t, TokenType>>{
            // Choose the longest keyword/operator that matches here so multi-character
            // operators (<=, >=, <>) and longer keywords (ORDER vs OR) win.  Only the
            // bucket for this character can match and it is sorted longest-first, so
            // the first match is the longest.
            for(const auto& [word, token]: TokenIndex.by_first_char[(unsigned char)std::toupper((unsigned char)c)]) {
                if(MatchesAt(line, index, word)) {
                    return std::make_pair(word.size(), token);
                }
            }
            return std::nullopt;
        }();

        if (result) {
            auto size = result.value().first;
            auto token = result.value().second;
            // A +/- immediately after a number's exponent 'E' (e.g. 1E-03) is part
            // of the number, not an operator, so keep accumulating it.
            if((token == PLUS || token == MINUS) && pending.size() >= 2) {
                char e = pending.back();
                char before = pending[pending.size() - 2];
                if((e == 'E' || e == 'e') && (isdigit((unsigned char)before) || before == '.')) {
                    add_pending(index, c);
                    index++;
                    continue;
                }
            }
            flush_pending();
            tokens.push_back(Token(token));
            index += size;
        } else {
            add_pending(index, c);
            index++;
        }
    }

    flush_pending();

    return tokens;
}

void PrintTokenized(const TokenList& tokens, int emphasize = -1)
{
    printf("%lu tokens: ", static_cast<unsigned long>(tokens.size()));
    int which = 0;
    for(const auto& t: tokens) {
        if(which == emphasize) {
            printf(" >>>");
        }
        switch(t.type) {
            case STRING_IDENTIFIER:
            case NUMBER_IDENTIFIER:
            {
                auto v = t.value;
                printf("%s ", std::get<std::string>(v).c_str());
                break;
            }
            case DOUBLE: {
                auto v = t.value;
                printf("%f ", std::get<double>(v));
                break;
            }
            case INTEGER: {
                auto v = t.value;
                printf("%d ", static_cast<int>(igr(v)));
                break;
            }
            case REMARK: {
                auto v = t.value;
                printf("REM%s ", std::get<std::string>(v).c_str());
                break;
            }
            case STRING: {
                auto v = t.value;
                printf("\"%s\" ", std::get<std::string>(v).c_str());
                break;
            }
            default: {
                printf("%s ", TokenTypeToStringMap[t.type]);
                break;
            }
        }
        if(which++ == emphasize) {
            printf("<<< ");
        }
    }
    printf("\n");
}

std::tuple<Value, Value> pop2(std::vector<Value>& operands)
{
    Value right = operands.back(); operands.pop_back();
    Value left = operands.back(); operands.pop_back();
    return {left, right};
}

template <typename Q>
auto pop(Q& queue)
{
    auto back = queue.back();
    queue.pop_back();
    return back;
}

void dump_state(const std::vector<TokenType>& operators, const std::vector<Value>& operands)
{
    printf("[");
    for(auto op: operators) { printf("\"%s\" ", TokenTypeToStringMap[op]); }
    printf("] (");
    for(auto op: operands) {
        if(is_num(op)) {
            printf("%f ", num(op));
        } else {
            printf("\"%s\" ", str(op).c_str());
        }
    }
    printf(")");
}

void dump_state(const std::vector<std::string>& operators, const std::vector<Value>& operands)
{
    printf("[");
    for(auto op: operators) { printf("\"%s\" ", op.c_str()); }
    printf("] (");
    for(auto op: operands) {
        if(is_num(op)) {
            printf("%f ", num(op));
        } else {
            printf("\"%s\" ", str(op).c_str());
        }
    }
    printf(")");
}

void dump_operators(const std::vector<std::string>& operators)
{
    printf("operators: ");
    for(auto op: operators) { printf("\"%s\" ", op.c_str()); }
    printf("\n");
}

void dump_operands(const std::vector<Value>& operands)
{
    printf("operands: ");
    for(auto op: operands) {
        if(is_num(op)) {
            printf("%f ", num(op));
        } else {
            printf("\"%s\" ", str(op).c_str());
        }
    }
    printf("\n");
}

Value evaluate(TokenType op, std::vector<Value>& operands)
{
    Value right = pop(operands);
    if(op == CLOSE_PAREN) {
        return right;
    } else if(op == OPEN_PAREN) {
        return right;
    } else if(op == POWER) {
        Value left = num(pop(operands));
        return pow(num(left), num(right));
    } else if(op == MULTIPLY) {
        Value left = pop(operands);
        return num(left) * num(right);
    } else if(op == DIVIDE) {
        Value left = pop(operands);
        return num(left) / num(right);
    } else if(op == PLUS) {
        Value left = pop(operands);
        return num(left) + num(right);
    } else if(op == MINUS) {
        Value left = pop(operands);
        return num(left) - num(right);
    } else if(op == TAB) {
	// TODO this is wrong, should add spaces from current location
	// to the specified next tab stop
        return std::string(static_cast<int>(num(right)), ' ');
    } else if(op == SIN) {
        return sin(num(right));
    } else if(op == COS) {
        return cos(num(right));
    } else if(op == TAN) {
        return tan(num(right));
    } else if(op == SGN) {
        return num(right) < 0.0 ? -1.0 : (num(right) > 0.0 ? 1.0 : 0.0);
    } else if(op == INT) {
        return trunc(num(right));
    } else if(op == RND) {
        return drand48();
    } else if(op == EXP) {
        return exp(num(right));
    } else if(op == LOG) {
        return log(num(right));
    } else {
        printf("internal error\n");
        abort();
    }
}

std::optional<TokenType> is_operator(TokenIterator op, TokenIterator end)
{
    if((op < end) && (operator_tokens.count(op->type) > 0)) {
        return op->type;
    }
    return std::nullopt;
}

bool is_binary_operator(TokenType op)
{
    return binary_operators.count(op) > 0;
}

bool is_unary_operator(TokenType op)
{
    return unary_operators.count(op) > 0;
}

bool is_function(TokenType&word)
{
    return function_tokens.count(word) > 0;
}

bool is_command(TokenType&command)
{
    return commands.count(command) > 0;
}

// Should the pending operator op1 (top of the operator stack) be applied before
// the incoming operator op2?  Higher precedence always reduces first.  Equal
// precedence reduces left-to-right (left associative) so 8-4-2 == (8-4)-2, except
// POWER which is right associative so 2^3^2 == 2^(3^2).
bool is_higher_precedence(TokenType op1, TokenType op2)
{
    if(!is_binary_operator(op1) || !is_binary_operator(op2)) {
        return false;
    }
    int p1 = operator_precedence.at(op1);
    int p2 = operator_precedence.at(op2);
    if(p1 == p2) {
        return op2 != POWER;
    }
    return p1 > p2;
}

// Render a number the way BASIC PRINT does: integers without a decimal point,
// otherwise up to 6 significant digits with trailing zeros trimmed.
std::string format_basic_number(double d)
{
    // ::trunc, not std::trunc: this newlib/libstdc++ combination declares the
    // C99 math functions only at global scope.
    if(d == trunc(d) && std::abs(d) < 1e15) {
        return std::to_string(static_cast<int64_t>(d));
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", d);
    return std::string(buf);
}

std::string to_str(const Value& v)
{
    if(is_vref(v)) {
        auto ref = vref(v);
        std::string s = ref.name;
        if(ref.indices.size() > 0) {
            s = s + "(" + std::to_string(ref.indices[0]);
            for(auto it = ref.indices.begin() + 1; it < ref.indices.end(); it++) {
                s = s + ", " + std::to_string(*it);
            }
            s = s + ")";
        }
        return s;
    } else if(is_num(v)) {
        return format_basic_number(num(v));
    } else {
        return str(v);
    }
}

struct VariableValue
{
    // A variable can have both a scalar and an array value
    // e.g. A = 5: A(0) = 6: PRINT A, A(0) yields "5 6"

    Value scalar;
    std::vector<int32_t> sizes;
    std::vector<Value> array;

    void SetArraySizes(const std::vector<int32_t>& sizes_, const Value& init)
    {
        sizes = sizes_;
        int32_t size = 1;
        for(int32_t s: sizes) {
            size *= (s + 1);
        }
        array.resize(size, init);
    }

    VariableValue(const Value& v) :
        scalar(v)
    {
    }

    VariableValue(const Value& v, const std::vector<int32_t>& sizes, const Value& init) :
        scalar(v)
    {
        SetArraySizes(sizes, init);
    }

    VariableValue(const std::vector<int32_t>& sizes, const Value& init)
    {
        SetArraySizes(sizes, init);
    }
};

typedef std::unordered_map<std::string, VariableValue> VariableMap;

struct VariableReferenceBoundsError
{
    std::string var;
    int32_t index;
    int32_t size;
    VariableReferenceBoundsError(const std::string var, int32_t index, int32_t size) :
        var(var),
        index(index),
        size(size)
    {}
};

struct VariableDimensionError
{
    std::string var;
    int used;
    int expected;
    VariableDimensionError(const std::string var, int used, int expected) :
        var(var),
        used(used),
        expected(expected)
    {}
};

struct ExecutionError
{
    enum Type {
        TYPE_MISMATCH,
        NOT_IN_RUN_STATE,
        NOT_IN_DIRECT_STATE,
        LINE_NOT_FOUND,
        VARIABLE_NOT_FOUND,
    } type;
    std::string why;
    ExecutionError(Type type) :
        type(type)
    {}
    ExecutionError(const std::string& why, Type type) :
        type(type),
        why(why)
    {}
};

void AllocateVariable(const VariableReference& ref, VariableMap& variables);

Value EvaluateVariable(const VariableReference& ref, VariableMap& variables)
{
    auto iter = variables.find(ref.name);
    if(iter == variables.end()) {
        // Referencing an unset variable yields a default in BASIC (0, or "" for a
        // string); for an array this also creates it with default dimensions.
        AllocateVariable(ref, variables);
        iter = variables.find(ref.name);
    }

    auto& val = iter->second;
    if(ref.indices.empty()) {
        return val.scalar;
    }

    if(val.sizes.size() != ref.indices.size()) {
        throw VariableDimensionError(ref.name, ref.indices.size(), val.sizes.size());
    }

    int32_t index = 0;
    int32_t stride = 1;
    for(size_t i = 0; i < val.sizes.size(); i++){
        if(ref.indices[i] > val.sizes[i]) {
            throw VariableReferenceBoundsError(ref.name, ref.indices[i], val.sizes[i]);
        }
        index = index + ref.indices[i] * stride;
        stride = stride * val.sizes[i];
    }
    return val.array[index];
}

void AllocateVariable(const VariableReference& ref, VariableMap& variables)
{
    if(!ref.indices.empty()) {
        std::vector<int32_t> sizes;
        for(size_t i = 0; i < ref.indices.size(); i++) {
            sizes.push_back(10);
        }
        if(ref.name[ref.name.size() - 1] == '$') {
            variables.emplace(std::make_pair(ref.name, VariableValue("", sizes, "")));
        } else {
            variables.emplace(std::make_pair(ref.name, VariableValue(0.0, sizes, 0.0)));
        }
    } else {
        if(ref.name[ref.name.size() - 1] == '$') {
            variables.emplace(std::make_pair(ref.name, VariableValue("")));
        } else {
            variables.emplace(std::make_pair(ref.name, VariableValue(0.0)));
        }
    }
}

void DimensionVariable(const std::string& identifier, const std::vector<int32_t> sizes, VariableMap& variables)
{
    if(identifier[identifier.size() - 1] == '$') {
        variables.emplace(std::make_pair(identifier, VariableValue("", sizes, "")));
    } else {
        variables.emplace(std::make_pair(identifier, VariableValue(0.0, sizes, 0.0)));
    }
}

void AssignVariable(const VariableReference& ref, const Value& value, VariableMap& variables)
{
    auto iter = variables.find(ref.name);

    if(iter == variables.end()) {
        AllocateVariable(ref, variables);
    } else {
        VariableValue& vv = variables.at(ref.name);
        if(!ref.indices.empty()) {
            if(ref.indices.size() != vv.sizes.size()) {
                throw VariableDimensionError(ref.name, ref.indices.size(), vv.sizes.size());
            }
        }
    }

    VariableValue& vv = variables.at(ref.name);

    if(!ref.indices.empty()) {
        int32_t index = 0;
        int32_t stride = 1;
        for(size_t i = 0; i < vv.sizes.size(); i++){
            if(ref.indices[i] > vv.sizes[i]) {
                throw VariableReferenceBoundsError(ref.name, ref.indices[i], vv.sizes[i]);
            }
            index = index + ref.indices[i] * stride;
            stride = stride * vv.sizes[i];
        }
        vv.array[index] = value;
    } else {
        vv.scalar = value;
    }
}

// An active FOR loop.  The loop body begins at (line, resume_index) so NEXT can
// jump back to it; the comparison uses limit/step.
struct ForRecord
{
    std::string var;
    double limit;
    double step;
    int line;
    int resume_index;
};

// A pending GOSUB: where RETURN should continue (just after the GOSUB statement).
struct GosubRecord
{
    int line;
    int resume_index;
};

// A DEF FN user function: its formal parameters and the expression tokens to
// evaluate (a copy of that slice of the defining line, so it survives the line
// being edited or deleted).
struct UserFunction
{
    std::vector<std::string> params;
    TokenList expression;
};

// A stored program line: the source exactly as entered, for LIST and SAVE, plus
// the tokens it was tokenized to when it was entered.
struct ProgramLine
{
    std::string source;
    TokenList tokens;
};

struct State
{
    VariableMap variables;
    // Program lines are tokenized once, when they are entered, and stored as
    // source plus tokens; running a line never re-tokenizes it, and a line that
    // does not tokenize is reported and not stored.  Tokenizing is deterministic
    // and tokens are never mutated, so the token offsets FOR/GOSUB capture stay
    // valid.  See MaybeStoreProgramLine and ExecuteNextLine.
    std::map<int, ProgramLine> program;
    std::vector<ForRecord> for_stack;
    std::vector<GosubRecord> gosub_stack;
    std::unordered_map<std::string, UserFunction> functions;
    std::vector<Value> data;        // flattened DATA values, rebuilt at RUN
    size_t data_index{0};
    int current_line{-1};
    int goto_line{-1};
    int resume_index{0};            // token index at which to begin the current line
    int width{80};
    bool direct{true};
    int column{0};
};

// Has the user asked the running program to stop?  A whole line of console
// input arriving while a program runs is the BREAK request (the classic "press
// Return to stop"; a typed ^C rides along inside the discarded line).  The line
// is consumed here so it is not later mistaken for a direct command.
//
// Only completed lines count, because the firmware console is line-cooked: this
// cannot interrupt a program that is already blocked inside INPUT, where Ctrl-D
// (end of input) is the escape instead.
bool check_break()
{
    struct pollfd fds{ 0, POLLIN, 0 };
    if(poll(&fds, 1, 0) <= 0)
    {
        return false;
    }
    if(!(fds.revents & POLLIN))
    {
        return false;
    }

    // One read is enough: the line discipline returns a short read at the line
    // boundary, and 0 means the line was Ctrl-D (end of input).
    char discard[128];
    (void)read(0, discard, sizeof(discard));
    return true;
}

// Today's date as MM/DD/YYYY and the time of day as HH:MM:SS, both UTC.  The
// clock is the firmware's boot epoch plus uptime, so these are only as good as
// the epoch the ROM was built with until a real RTC is wired up.
std::string date_string()
{
    time_t now = time(nullptr);
    char buf[16];
    strftime(buf, sizeof(buf), "%m/%d/%Y", gmtime(&now));
    return std::string(buf);
}

std::string time_string()
{
    time_t now = time(nullptr);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", gmtime(&now));
    return std::string(buf);
}

// Seconds since midnight UTC, with a fraction, for TIMER -- the classic BASIC
// meaning.  (Seconds since the epoch would be the obvious thing to return, but
// PRINT renders numbers to six significant digits, which at 1.8e9 cannot show a
// difference of a few seconds.)  The useful resolution is the 10 ms firmware
// tick, whatever gettimeofday() claims.
constexpr long seconds_per_day = 24 * 60 * 60;

double timer_seconds()
{
    struct timeval tv;
    if(gettimeofday(&tv, nullptr) != 0)
    {
        return 0.0;
    }
    return static_cast<double>(tv.tv_sec % seconds_per_day) +
        static_cast<double>(tv.tv_usec) / 1000000.0;
}

namespace Console
{
    void Print(const std::string& str, State& state)
    {
        std::cout << str;
        size_t newline_at = str.find_last_of('\n');
        if(newline_at == std::string::npos) {
            state.column += str.size();
        } else {
            // After a newline the column resets to the count of characters that
            // follow the last newline in this string.
            state.column = str.size() - newline_at - 1;
        }
    }

    void Tab(int tabstop, State& state)
    {
        int needed = tabstop - state.column % tabstop;
        std::cout << std::string(needed, ' ');
        state.column += needed;
    }
}


// Thrown when check_break() sees the user's BREAK request.  Like the other
// error types it unwinds all the way out to the main loop, which reports it and
// returns to direct mode.
struct BreakException
{
};

struct ParseError
{
    enum Type {
        TRAILING_TOKENS,
        UNEXPECTED_END,
        UNEXPECTED,
        EXPECTED_TOKEN,
        EXPECTED_RULE,
    } type;
    TokenType expected_token_type{TOKENTYPE_END};
    TokenList tokens;
    int token{-1};
    std::string expected_term;

    ParseError(TokenList tokens, TokenType expected_token, int token) :
        type(EXPECTED_TOKEN),
        expected_token_type(expected_token),
        tokens(tokens),
        token(token)
    {}

    ParseError(TokenList tokens, const std::string& expected_term, [[maybe_unused]] int token) :
        type(EXPECTED_RULE),
        tokens(tokens),
        expected_term(expected_term)
    {}

    // UNEXPECTED or TRAILING_TOKENS
    ParseError(TokenList tokens, Type type, int token) :
        type(type),
        tokens(tokens),
        token(token)
    {}

    ParseError(TokenList tokens) :
        type(UNEXPECTED_END),
        tokens(tokens)
    {}

    ParseError(Type type) :
        type(type)
    {}
};

std::optional<Token> ParseOptional([[maybe_unused]] const TokenList& tokens, TokenIterator& cur, TokenIterator& end, [[maybe_unused]] State& state, const std::set<TokenType>& expect)
{
    if((cur < end) && (expect.count(cur->type) > 0)) {
        return *cur++;
    }
    return {};
}

std::optional<Token> ParseAny(const TokenList& tokens, TokenIterator& cur, [[maybe_unused]] TokenIterator& end, [[maybe_unused]] State& state, const std::set<TokenType>& expect)
{
    if(cur >= tokens.end()) { return {}; }
    if(expect.count(cur->type) > 0) {
        return (cur++)->type;
    }
    return {};
}

// If next token is "expect", then increment pointer and return matched token.
std::optional<Token> ParseOne(const TokenList& tokens, TokenIterator& cur, [[maybe_unused]] TokenIterator& end, [[maybe_unused]] State& state, TokenType expect)
{
    if(cur >= tokens.end()) { return {}; }
    if(cur->type == expect) {
        auto matched = *cur;
        cur++;
        return matched;
    }
    return {};
}

// Every caller passes a braced list of a handful of token types.  An
// initializer_list keeps that list on the stack; a std::set parameter allocated
// (and freed) a node per element on every call.
bool IsOneOf(TokenType type, std::initializer_list<TokenType> expect)
{
    for(TokenType expected: expect) {
        if(expected == type) {
            return true;
        }
    }
    return false;
}

// identifier ::= NUMBER_IDENTIFIER | STRING_IDENTIFIER // returns optional std::string
std::optional<Value> ParseIdentifier([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end) { return {}; }
    if(IsOneOf(cur_->type, {NUMBER_IDENTIFIER, STRING_IDENTIFIER})) {
        return cur_++->value;
    }
    return {};
}

// number ::= DOUBLE | INTEGER // returns optional Value
std::optional<Value> ParseNumber([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end) { return {}; }
    if(IsOneOf(cur_->type, {DOUBLE, INTEGER})) {
        return cur_++->value;
    }
    return {};
}

// unary-op ::= (PLUS | MINUS | NOT) // returns TokenType
std::optional<TokenType> ParseUnaryOp([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end) { return {}; }
    if(IsOneOf(cur_->type, {PLUS, MINUS, NOT})) {
        return cur_++->type;
    }
    return {};
}

// binary-operator ::= POWER | MULTIPLY | DIVIDE | PLUS | MINUS | LESS_THAN | GREATER_THAN | LESS_THAN_EQUAL | GREATER_THAN_EQUAL | EQUAL | NOT_EQUAL | AND | OR // returns TokenType
std::optional<TokenType> ParseBinaryOperator([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end) { return {}; }
    if(IsOneOf(cur_->type, {POWER, MULTIPLY, DIVIDE, PLUS, MINUS, LESS_THAN, GREATER_THAN, LESS_THAN_EQUAL, GREATER_THAN_EQUAL, EQUAL, NOT_EQUAL, AND, OR})) {
        return cur_++->type;
    }
    return {};
}

// integer-list ::= INTEGER {COMMA INTEGER} // returns std::vector<int32_t>
std::optional<std::vector<int32_t>> ParseIntegerList([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    std::vector<int32_t> integers;
    if(cur_ >= end) { return {}; }
    if(cur_->type != INTEGER) {
        return {};
    }
    auto cur = cur_;
    integers.push_back(igr(cur++->value));
    while((cur < end) && (cur->type == COMMA)) {
        cur++;
        if(cur->type != INTEGER) {
            return {};
        }
        integers.push_back(igr(cur++->value));
    }
    cur_ = cur;
    return integers;
}

// number-identifier-list ::= NUMBER_IDENTIFIER {COMMA NUMBER_IDENTIFIER} // returns std::vector<Token>
std::optional<std::vector<Value>> ParseNumberIdentifierList([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    std::vector<Value> identifiers;
    if(cur_ >= end) { return {}; }
    if(cur_->type != NUMBER_IDENTIFIER) {
        return {};
    }
    auto cur = cur_;
    identifiers.push_back(cur++->value);
    while((cur < end) && (cur->type == COMMA)) {
        cur++;
        if(cur->type != NUMBER_IDENTIFIER) {
            return {};
        }
        identifiers.push_back(cur++->value);
    }
    cur_ = cur;
    return identifiers;
}

// numeric-function-name ::= ABS | ATN | COS | EXP | INT | LOG | RND | SGN | SIN | SQR | TAN | TAB | CHR | STR // returns TokenType
std::optional<TokenType> ParseNumericFunctionName([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end) { return {}; }
    if(IsOneOf(cur_->type, {ABS, ATN, COS, EXP, INT, LOG, RND, SGN, SIN, SQR, TAN, TAB, CHR, STR})) {
        return cur_++->type;
    }
    return {};
}

std::optional<double> ParseNumericExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);

// numeric-function ::= numeric-function-name OPEN_PAREN numeric-expression CLOSE_PAREN  // returns TokenType
std::optional<Value> ParseNumericFunction(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;

    auto function = ParseNumericFunctionName(tokens, cur, end);
    if(!function.has_value()) {
        return {};
    }

    if(!ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
        return {};
    }

    auto argument = ParseNumericExpression(tokens, cur, end, state);
    if(!argument.has_value()) {
        return {};
    }

    if(!ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
        return {};
    }

    Value v;
    switch(*function) {
        case ABS:
            v = std::abs(*argument);  // std::, or this picks up C's int abs()
            break;
        case ATN:
            v = atan(*argument);
            break;
        case COS:
            v = cos(*argument);
            break;
        case EXP:
            v = exp(*argument);
            break;
        case INT:
            v = floor(*argument);  // BASIC INT rounds toward negative infinity
            break;
        case LOG:
            v = log(*argument);
            break;
        case RND:
            v = drand48();
            break;
        case SGN:
            v = (*argument) < 0.0 ? -1.0 : ((*argument) > 0.0 ? 1.0 : 0.0);
            break;
        case SIN:
            v = sin(*argument);
            break;
        case SQR:
            v = sqrt(*argument);
            break;
        case TAN:
            v = tan(*argument);
            break;
        case TAB: {
            // Pad with spaces to reach absolute column *argument (1-based, matching
            // state.column which counts characters printed on the current line).
            int needed = static_cast<int>(*argument) - state.column;
            v = std::string(needed > 0 ? needed : 0, ' ');
            break;
        }
        case CHR: {
            char c = static_cast<int>(*argument);
            v = std::string(1, c);
            break;
        }
        case STR:
            v = format_basic_number(*argument);
            break;
        default:
            // notreached
            break;
    }

    cur_ = cur;
    return v;
}

std::optional<Value> ParseExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);
std::optional<std::string> ParseStringExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);

// string-function ::= (LEN | VAL) OPEN_PAREN string-expression CLOSE_PAREN
//                   | (LEFT$ | RIGHT$) OPEN_PAREN string-expression COMMA numeric-expression CLOSE_PAREN
//                   | MID$ OPEN_PAREN string-expression COMMA numeric-expression [COMMA numeric-expression] CLOSE_PAREN
// LEN/VAL return a number; the others return a string.  String indices are 1-based.
std::optional<Value> ParseStringFunction(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || !IsOneOf(cur->type, {LEN, VAL, LEFT, RIGHT, MID})) {
        return {};
    }
    TokenType fn = cur++->type;

    if(!ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
        return {};
    }
    auto s = ParseStringExpression(tokens, cur, end, state);
    if(!s) {
        return {};
    }

    Value v;
    if(fn == LEN) {
        v = static_cast<double>(s->size());
    } else if(fn == VAL) {
        v = strtod(s->c_str(), nullptr);
    } else if(fn == LEFT || fn == RIGHT) {
        if(!ParseOne(tokens, cur, end, state, COMMA)) {
            return {};
        }
        auto n = ParseNumericExpression(tokens, cur, end, state);
        if(!n) {
            return {};
        }
        int count = static_cast<int>(*n);
        count = std::max(0, std::min(count, static_cast<int>(s->size())));
        v = (fn == LEFT) ? s->substr(0, count) : s->substr(s->size() - count);
    } else {  // MID$
        if(!ParseOne(tokens, cur, end, state, COMMA)) {
            return {};
        }
        auto startn = ParseNumericExpression(tokens, cur, end, state);
        if(!startn) {
            return {};
        }
        int start = std::max(1, static_cast<int>(*startn));  // 1-based
        int len = -1;
        if(ParseOne(tokens, cur, end, state, COMMA)) {
            auto lenn = ParseNumericExpression(tokens, cur, end, state);
            if(!lenn) {
                return {};
            }
            len = static_cast<int>(*lenn);
        }
        std::string result;
        if(start <= static_cast<int>(s->size())) {
            size_t from = start - 1;
            result = (len < 0) ? s->substr(from) : s->substr(from, len);
        }
        v = result;
    }

    if(!ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
        return {};
    }
    cur_ = cur;
    return v;
}

// user-function ::= FN NUMBER_IDENTIFIER OPEN_PAREN expression {COMMA expression} CLOSE_PAREN
// Evaluates a function defined with DEF FN by binding the arguments to the formal
// parameters (saving and restoring any same-named variables) and evaluating the
// stored expression tokens.
std::optional<Value> ParseUserFunction(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(!ParseOne(tokens, cur, end, state, FN)) {
        return {};
    }
    auto name = ParseIdentifier(tokens, cur, end);
    if(!name) {
        return {};
    }
    auto it = state.functions.find(str(*name));
    if(it == state.functions.end()) {
        throw ExecutionError("FN " + str(*name), ExecutionError::VARIABLE_NOT_FOUND);
    }
    const UserFunction& fn = it->second;

    if(!ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
        return {};
    }
    std::vector<Value> args;
    do {
        auto arg = ParseExpression(tokens, cur, end, state);
        if(!arg) {
            return {};
        }
        args.push_back(*arg);
    } while(ParseOne(tokens, cur, end, state, COMMA));
    if(!ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
        return {};
    }
    if(args.size() != fn.params.size()) {
        throw ExecutionError("FN argument count", ExecutionError::TYPE_MISMATCH);
    }

    // Bind parameters, remembering any shadowed variables so we can restore them.
    std::vector<std::pair<std::string, std::optional<VariableValue>>> saved;
    for(size_t i = 0; i < fn.params.size(); i++) {
        const std::string& p = fn.params[i];
        auto vit = state.variables.find(p);
        saved.push_back({p, vit != state.variables.end()
                                ? std::optional<VariableValue>(vit->second)
                                : std::nullopt});
        AssignVariable(VariableReference(p, {}), args[i], state.variables);
    }

    TokenIterator ecur = fn.expression.begin();
    auto result = ParseExpression(fn.expression, ecur, fn.expression.end(), state);

    for(auto& [p, val] : saved) {
        if(val.has_value()) {
            state.variables.insert_or_assign(p, *val);
        } else {
            state.variables.erase(p);
        }
    }

    if(!result) {
        throw ExecutionError("FN body", ExecutionError::TYPE_MISMATCH);
    }
    cur_ = cur;
    return *result;
}

// nullary-function ::= TIMER | DATE$ | TIME$ // evaluates, returns Value
// These take no argument at all -- not even parentheses, unlike RND(x) -- so
// they are recognized as bare terms rather than through ParseNumericFunction.
std::optional<Value> ParseNullaryFunction(TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end)
    {
        return {};
    }

    switch(cur_->type)
    {
        case TIMER:
            cur_++;
            return timer_seconds();
        case DATE:
            cur_++;
            return date_string();
        case TIME:
            cur_++;
            return time_string();
        default:
            return {};
    }
}

// function ::= nullary-function | numeric-function | string-function | user-function // evaluates, returns Value
std::optional<Value> ParseFunction(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(auto value = ParseNullaryFunction(cur_, end)) {
        return value;
    }
    if(auto value = ParseNumericFunction(tokens, cur_, end, state)) {
        return value;
    }
    if(auto value = ParseStringFunction(tokens, cur_, end, state)) {
        return value;
    }
    if(auto value = ParseUserFunction(tokens, cur_, end, state)) {
        return value;
    }
    return {};
}


// integer ::= INTEGER // returns optional int32_t
std::optional<Value> ParseInteger([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end)
{
    if(cur_ >= end) { return {}; }
    if(cur_->type != INTEGER) {
        return {};
    }
    return cur_++->value;
}

bool ParseSingleWordStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, [[maybe_unused]] State& state, TokenType expected)
{
    auto cur = cur_;

    if(cur >= end || cur_->type != expected) {
        return false;
    }
    cur++;

    // Committed from here, must emit parse error if can't match
    if(cur >= end) {
        cur_ = cur;
        return true;
    }

    if(cur->type == COLON) {
        cur++;
        cur_ = cur;
        return true;
    }

    throw ParseError(tokens, ParseError::UNEXPECTED, cur - tokens.begin());
}


// end-statement ::= END (COLON | end)// returns void
bool ParseEndStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    bool succeeded = ParseSingleWordStatement(tokens, cur_, end, state, END);
    if(succeeded) {
        if(state.direct) {
            throw ExecutionError("END command", ExecutionError::NOT_IN_DIRECT_STATE);
        }
        state.direct = true;  // halt the program, return to command mode
    }
    return succeeded;
}

// clear-statement ::= CLEAR [integer] (COLON | end) // returns void
// CLEAR erases all variables.  An optional numeric argument (the string/array space
// to reserve on real hardware, e.g. STARTREK's "CLEAR 600") is accepted and ignored.
bool ParseClearStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != CLEAR) {
        return false;
    }
    cur++;

    if(cur < end && cur->type != COLON) {
        if(!ParseNumericExpression(tokens, cur, end, state)) {
            throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
        }
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }

    state.variables.clear();
    cur_ = cur;
    return true;
}

// run-statement ::= RUN (COLON | end)// returns void
// Gather every DATA value in the program (in line order) into a flat list that
// READ consumes sequentially.  Rebuilt at each RUN.
void BuildDataList(State& state)
{
    state.data.clear();
    state.data_index = 0;
    for(const auto& [line_number, stored] : state.program) {
        const TokenList& tokens = stored.tokens;
        for(size_t i = 0; i < tokens.size(); ) {
            if(tokens[i].type != DATA) {
                i++;
                continue;
            }
            i++;  // skip the DATA keyword; the rest of the line is data values
            while(i < tokens.size()) {
                bool negate = false;
                if(tokens[i].type == MINUS) {
                    negate = true;
                    i++;
                }
                if(i >= tokens.size()) {
                    break;
                }
                const Token& t = tokens[i];
                if(t.type == INTEGER || t.type == DOUBLE) {
                    double d = num(t.value);
                    state.data.push_back(negate ? -d : d);
                    i++;
                } else if(t.type == STRING) {
                    state.data.push_back(t.value);
                    i++;
                } else {
                    break;  // e.g. a COLON ending the DATA statement
                }
                if(i < tokens.size() && tokens[i].type == COMMA) {
                    i++;
                } else {
                    break;
                }
            }
        }
    }
}

// Clear run-time state and begin executing at the first program line.  Like
// classic RUN, this resets variables and the FOR/GOSUB stacks; DEF FN functions
// re-register as their lines run, and DATA is gathered up front for READ.
void StartRun(State& state)
{
    if(state.program.empty()) {
        return;
    }
    state.variables.clear();
    state.for_stack.clear();
    state.gosub_stack.clear();
    state.functions.clear();
    state.resume_index = 0;
    state.column = 0;
    BuildDataList(state);
    state.current_line = state.program.begin()->first;
    state.direct = false;
}

bool ParseRunStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    bool succeeded = ParseSingleWordStatement(tokens, cur_, end, state, RUN);
    if(succeeded) {
        if(!state.direct) {
            throw ExecutionError("RUN command", ExecutionError::NOT_IN_DIRECT_STATE);
        }
        StartRun(state);
    }
    return succeeded;
}

// stop-statement ::= STOP (COLON | end) // returns void
bool ParseStopStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    bool succeeded = ParseSingleWordStatement(tokens, cur_, end, state, STOP);
    if(succeeded) {
        if(state.direct) {
            throw ExecutionError("STOP command", ExecutionError::NOT_IN_DIRECT_STATE);
        }
        printf("STOP at line %d\n", state.current_line);
        state.direct = true;  // halt the program, return to command mode
    }
    return succeeded;
}

// goto-statement ::= GOTO integer (COLON | end) // returns void
std::optional<int32_t> ParseGotoStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(cur_ >= end) { return {}; }

    if(cur_->type != GOTO) {
        return {};
    }

    // Committed from here, must emit parse error if can't match

    auto cur = cur_ + 1;
    if(cur >= end) {
        throw ParseError(tokens);
    }

    if(cur->type != INTEGER) {
        throw ParseError(tokens, INTEGER, cur - tokens.begin());
    }

    if(state.direct) {
        throw ExecutionError("GOTO command", ExecutionError::NOT_IN_DIRECT_STATE);
    }
    int goto_line = igr(cur++->value);

    if(cur >= end || ParseOne(tokens, cur, end, state, COLON)) {
        cur_ = cur;
        return state.goto_line = goto_line;
    }

    throw ParseError(tokens, ParseError::UNEXPECTED, cur - tokens.begin());
}

std::optional<VariableReference> ParseVariableReference(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);

std::optional<Value> ParseParenExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);

// term ::= number | STRING | variable-reference | function | paren-expression // returns Value
std::optional<Value> ParseTerm(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(cur_ >= end) { return {}; }

    if(auto results = ParseNumber(tokens, cur_, end)) {
        return *results;
    }

    if(cur_->type == STRING) {
        return cur_++->value;
    }

    if(auto results = ParseVariableReference(tokens, cur_ ,end, state)) {
        return EvaluateVariable(*results, state.variables);
    }

    if(auto results = ParseFunction(tokens, cur_, end, state)) {
        return results;
    }

    if(auto results = ParseParenExpression(tokens, cur_, end, state)) {
        return results;
    }

    return {};
}

// unary-operation ::= {unary-op} term // evaluates using unary-ops, returns Value
std::optional<Value> ParseUnaryOperation(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(cur_ >= end) { return {}; }

    auto cur = cur_;
    std::vector<TokenType> operators;
    while(auto unary_op = ParseUnaryOp(tokens, cur, end)) {
        operators.push_back(*unary_op);
    }

    if(auto value = ParseTerm(tokens, cur, end, state)) {
        if(!value) {
            return {};
        }
        if(!is_num(*value)) {
            if(!operators.empty()) {
                throw ExecutionError(ExecutionError::TYPE_MISMATCH);
            }
            cur_ = cur;
            return value;
        }
        double v = num(*value);
        for(auto it = operators.rbegin(); it != operators.rend(); it++) {
            switch(*it) {
                case TokenType::PLUS: break;
                case TokenType::MINUS: v = -v; break;
                case TokenType::NOT: v = -1 - static_cast<int>(trunc(v)); break;
                default: break;
            }
        }
        cur_ = cur;
        return v;
    }

    return {};
}

std::optional<Value> ParseExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);
std::optional<int32_t> ParseIntegerExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);
std::optional<std::string> ParseStringExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);

// paren-expression ::= OPEN_PAREN expression CLOSE_PAREN // returns Value
std::optional<Value> ParseParenExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;

    if(!ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
        return {};
    }

    auto results = ParseExpression(tokens, cur, end, state);
    if(!results) {
        return {};
    }

    if(!ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
        return {};
    }

    cur_ = cur;
    return results;
}

// print-statement ::= PRINT {COMMA | SEMICOLON | expression} (COLON | END) // returns void
bool ParsePrintStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(cur_ >= end) { return false; }

    if(cur_->type != PRINT) {
        return false;
    }
    // Committed from here, must emit parse error if can't match
    auto cur = cur_ + 1;

    bool lastWasConcat = false;

    while(cur < end && cur->type != COLON) {
        if(cur->type == COMMA) {
            Console::Tab(20, state);
            lastWasConcat = true;
            cur++;
        } else if(cur->type == SEMICOLON) {
            // skip
            lastWasConcat = true;
            cur++;
        } else if(auto results = ParseExpression(tokens, cur, end, state)) {
            Console::Print(to_str(*results), state);
            lastWasConcat = false;
        } else {
            throw ParseError(tokens, ParseError::UNEXPECTED, cur - tokens.begin());
        }
    } 

    if(!lastWasConcat) {
        Console::Print("\n", state);
    }

    if(cur->type == COLON) {
        cur++;
    }

    cur_ = cur;
    return true;
}

// dim-statement ::= DIM identifier OPEN_PAREN integer-list CLOSE_PAREN {COMMA OPEN_PAREN integer-list CLOSE_PAREN} (COLON | end)
bool ParseDimStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(cur_ >= end) { return false; }

    if(cur_->type != DIM) {
        return false;
    }
    // Committed from here, must emit parse error if can't match
    auto cur = cur_ + 1;

    bool keepgoing = true;
    while(keepgoing) {
        auto identifier = ParseIdentifier(tokens, cur, end);
        if(!ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
            throw ParseError(tokens, OPEN_PAREN, cur - tokens.begin());
        }

        auto dimensions = ParseIntegerList(tokens, cur, end);
        if(!dimensions) {
            throw ParseError(tokens, "integer-list", cur - tokens.begin());
        }

        if(!ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
            throw ParseError(tokens, CLOSE_PAREN, cur - tokens.begin());
        }

        DimensionVariable(str(*identifier), *dimensions, state.variables);

        keepgoing = ParseOne(tokens, cur, end, state, COMMA).has_value();
    } 

    if(cur < end && cur->type == COLON) {
        cur++;
    }

    cur_ = cur;
    return true;
}

// rem-statement ::= [REM] variable-reference EQUAL expression (COLON | end) // returns void
bool ParseRemStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, [[maybe_unused]] State& state)
{
    auto cur = cur_;
    if(cur >= end) { return false; }

    if(cur->type != REMARK) {
        return {};
    }

    cur++;
    if(cur < end) {
        throw ParseError(tokens, ParseError::TRAILING_TOKENS, cur - tokens.begin());
    }

    cur_ = cur;
    return true;
}

// let-statement ::= [LET] variable-reference EQUAL expression (COLON | end) // returns void
bool ParseLetStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end) { return false; }

    bool committed_to_let = ParseOne(tokens, cur, end, state, LET).has_value();
    // If we saw "LET", we are committed from here, must emit parse error if can't match

    auto ref = ParseVariableReference(tokens, cur, end, state);
    if(!ref) {
        if(committed_to_let) {
            throw ParseError(tokens, "variable-reference", cur - tokens.begin());
        } else {
            return false;
        }
    }
    // Committed from here, must emit parse error if can't match

    if(!ParseOne(tokens, cur, end, state, EQUAL)) {
        throw ParseError(tokens, EQUAL, cur - tokens.begin());
    }

    auto value = ParseExpression(tokens, cur, end, state);
    if(!value) {
        throw ParseError(tokens, "expression", cur - tokens.begin());
    }
    if((ref->name[ref->name.size() - 1] == '$') && !is_str(*value)) {
        throw ExecutionError(ExecutionError::TYPE_MISMATCH);
    }
    if((ref->name[ref->name.size() - 1] != '$') && !is_num(*value)) {
        throw ExecutionError(ExecutionError::TYPE_MISMATCH);
    }

    if(cur->type == COLON) {
        cur++;
    }

    AssignVariable(*ref, *value, state.variables);

    cur_ = cur;
    return true;
}

// variable-reference-list ::= variable-reference {COMMA | variable-reference} // returns std::vector<VariableReference>
std::optional<std::vector<VariableReference>> ParseVariableReferenceList(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    std::vector<VariableReference> refs;

    bool keepgoing = false;
    do {
        auto reference = ParseVariableReference(tokens, cur, end, state);
        if(!reference) {
            return {};
        }
        refs.push_back(*reference);
        keepgoing = ParseOne(tokens, cur, end, state, COMMA).has_value();
    } while(keepgoing);

    cur_ = cur;
    return refs;
}

// Split one line of typed input into comma-separated, whitespace-trimmed fields.
std::vector<std::string> SplitInputFields(const std::string& input)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while(true) {
        size_t comma = input.find(',', start);
        std::string f = input.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t a = f.find_first_not_of(" \t");
        size_t b = f.find_last_not_of(" \t");
        fields.push_back(a == std::string::npos ? "" : f.substr(a, b - a + 1));
        if(comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return fields;
}

// input-statement ::= INPUT [STRING SEMICOLON] variable-reference-list (COLON | end) // returns void
bool ParseInputStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    if(cur_ >= end || cur_->type != INPUT) {
        return false;
    }
    // Committed from here, must emit parse error if can't match
    auto cur = cur_ + 1;

    std::string prompt_str;
    if(auto prompt = ParseOne(tokens, cur, end, state, STRING)) {
        prompt_str = str(prompt->value);
        if(!ParseOne(tokens, cur, end, state, SEMICOLON)) {
            throw ParseError(tokens, SEMICOLON, cur - tokens.begin());
        }
    }

    auto refs = ParseVariableReferenceList(tokens, cur, end, state);
    if(!refs) {
        throw ParseError(tokens, "variable-reference-list", cur - tokens.begin());
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }

    if(!prompt_str.empty()) {
        std::cout << prompt_str;
    }

    // Read input lazily, splitting on commas; if the user supplies too few values
    // for the variable list, prompt again with "? " (classic BASIC behavior).
    std::vector<std::string> fields;
    size_t field_index = 0;
    auto next_field = [&]() -> std::string {
        while(field_index >= fields.size()) {
            std::cout << "? ";
            std::cout.flush();
            std::string input;
            if(!std::getline(std::cin, input)) {
                // Ctrl-D is how a blocked INPUT is escaped, and the firmware
                // console delivers that end-of-file exactly once -- but both
                // layers above latch it.  Clear cin's eofbit and stdin's EOF
                // indicator (cin is synced with stdio and reads through it, and
                // the direct-mode prompt below uses fgets(stdin) directly), or
                // the interpreter quits instead of returning to the prompt.
                std::cin.clear();
                clearerr(stdin);
                throw ExecutionError("end of input during INPUT", ExecutionError::NOT_IN_RUN_STATE);
            }
            fields = SplitInputFields(input);
            field_index = 0;
        }
        return fields[field_index++];
    };

    for(const auto& ref : *refs) {
        std::string field = next_field();
        if(ref.name.back() == '$') {
            AssignVariable(ref, Value(field), state.variables);
        } else {
            AssignVariable(ref, Value(strtod(field.c_str(), nullptr)), state.variables);
        }
    }
    state.column = 0;  // the user pressed return, so we're at the start of a line

    cur_ = cur;
    return true;
}


// statement ::= ( print-statement | let-statement | input-statement | dim-statement | if-statement | for-statement | next-statement | on-statement | goto-statement | gosub-statement | wait-statement | width-statement | order-statement | read-statement | data-statement | deffn-statement | return-statement | end-statement | clear-statement | run-statement | stop-statement ) // returns void
void ParseStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state);

// if-statement ::= IF expression (THEN | GOTO) (integer | statement-list) [ELSE (integer | statement-list)]
// A bare integer clause means GOTO that line.  On a false condition with no ELSE the
// rest of the line is skipped (standard BASIC: the whole line is the THEN-clause).
bool ParseIfStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != IF) {
        return false;
    }
    cur++;

    auto cond = ParseExpression(tokens, cur, end, state);
    if(!cond) {
        throw ParseError(tokens, "expression", cur - tokens.begin());
    }
    bool truth = is_num(*cond) ? (num(*cond) != 0.0) : !str(*cond).empty();

    // The then-action follows THEN, or a bare GOTO as shorthand.
    if(cur < end && cur->type == THEN) {
        cur++;
    } else if(cur < end && cur->type == GOTO) {
        // leave the GOTO token for the clause to parse
    } else {
        throw ParseError(tokens, THEN, cur - tokens.begin());
    }

    // The THEN-clause extends to a top-level ELSE or to the end of the line.
    TokenIterator else_pos = cur;
    while(else_pos < end && else_pos->type != ELSE) {
        else_pos++;
    }
    TokenIterator then_begin = cur;
    TokenIterator then_end = else_pos;
    TokenIterator else_begin = (else_pos < end) ? else_pos + 1 : end;

    auto run_clause = [&](TokenIterator b, TokenIterator e) {
        if(b < e && b->type == INTEGER) {
            state.goto_line = igr(b->value);   // bare line number == GOTO
            return;
        }
        TokenIterator c = b;
        while(c < e && state.goto_line == -1 && !state.direct) {
            ParseStatement(tokens, c, e, state);
        }
    };

    if(truth) {
        run_clause(then_begin, then_end);
    } else if(else_begin < end) {
        run_clause(else_begin, end);
    }

    cur_ = end;  // the IF owns the rest of the line
    return true;
}

// Scan forward from the body of a FOR (at from_line/from_index) for its matching
// NEXT, returning the (line, token-index) just past that NEXT.  Used to skip a loop
// that runs zero times.  Nested FOR/NEXT are balanced by depth.
std::pair<int, int> FindMatchingNext(State& state, int from_line, int from_index)
{
    int depth = 1;
    auto it = state.program.find(from_line);
    bool first = true;
    for(; it != state.program.end(); ++it) {
        const TokenList& tokens = it->second.tokens;
        size_t i = first ? static_cast<size_t>(from_index) : 0;
        first = false;
        for(; i < tokens.size(); i++) {
            if(tokens[i].type == FOR) {
                depth++;
            } else if(tokens[i].type == NEXT) {
                depth--;
                if(depth == 0) {
                    size_t j = i + 1;
                    if(j < tokens.size() && tokens[j].type == NUMBER_IDENTIFIER) {
                        j++;  // skip optional loop variable
                    }
                    if(j < tokens.size() && tokens[j].type == COLON) {
                        j++;
                    }
                    return { it->first, static_cast<int>(j) };
                }
            }
        }
    }
    throw ExecutionError("FOR without NEXT", ExecutionError::LINE_NOT_FOUND);
}

// for-statement ::= FOR NUMBER_IDENTIFIER EQUAL expression TO expression [STEP expression] (COLON | end)
bool ParseForStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != FOR) {
        return false;
    }
    if(state.direct) {
        throw ExecutionError("FOR command", ExecutionError::NOT_IN_DIRECT_STATE);
    }
    cur++;

    auto var = ParseIdentifier(tokens, cur, end);
    if(!var || str(*var).back() == '$') {
        throw ParseError(tokens, "loop variable", cur - tokens.begin());
    }
    if(!ParseOne(tokens, cur, end, state, EQUAL)) {
        throw ParseError(tokens, EQUAL, cur - tokens.begin());
    }
    auto start = ParseNumericExpression(tokens, cur, end, state);
    if(!start) {
        throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
    }
    if(!ParseOne(tokens, cur, end, state, TO)) {
        throw ParseError(tokens, TO, cur - tokens.begin());
    }
    auto limit = ParseNumericExpression(tokens, cur, end, state);
    if(!limit) {
        throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
    }
    double step = 1.0;
    if(ParseOne(tokens, cur, end, state, STEP)) {
        auto s = ParseNumericExpression(tokens, cur, end, state);
        if(!s) {
            throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
        }
        step = *s;
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }

    AssignVariable(VariableReference(str(*var), {}), *start, state.variables);
    int body_index = cur - tokens.begin();

    // If the loop is already complete, skip past the matching NEXT entirely.
    bool done = (step >= 0) ? (*start > *limit) : (*start < *limit);
    if(done) {
        auto [nline, nindex] = FindMatchingNext(state, state.current_line, body_index);
        state.goto_line = nline;
        state.resume_index = nindex;
        cur_ = end;
        return true;
    }

    state.for_stack.push_back({ str(*var), *limit, step, state.current_line, body_index });
    cur_ = cur;
    return true;
}

// next-statement ::= NEXT [NUMBER_IDENTIFIER] (COLON | end)
bool ParseNextStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != NEXT) {
        return false;
    }
    cur++;
    std::optional<std::string> var;
    if(auto id = ParseIdentifier(tokens, cur, end)) {
        var = str(*id);
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }

    if(state.for_stack.empty()) {
        throw ExecutionError("NEXT without FOR", ExecutionError::NOT_IN_RUN_STATE);
    }
    // A named NEXT may close inner loops that were left open.
    if(var) {
        while(!state.for_stack.empty() && state.for_stack.back().var != *var) {
            state.for_stack.pop_back();
        }
        if(state.for_stack.empty()) {
            throw ExecutionError("NEXT " + *var + " without FOR", ExecutionError::NOT_IN_RUN_STATE);
        }
    }

    ForRecord rec = state.for_stack.back();
    double next = num(EvaluateVariable(VariableReference(rec.var, {}), state.variables)) + rec.step;
    AssignVariable(VariableReference(rec.var, {}), next, state.variables);

    bool keep_going = (rec.step >= 0) ? (next <= rec.limit) : (next >= rec.limit);
    if(keep_going) {
        state.goto_line = rec.line;
        state.resume_index = rec.resume_index;
        cur_ = end;
    } else {
        state.for_stack.pop_back();
        cur_ = cur;  // loop finished; continue after NEXT
    }
    return true;
}

// gosub-statement ::= GOSUB integer (COLON | end)
bool ParseGosubStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != GOSUB) {
        return false;
    }
    if(state.direct) {
        throw ExecutionError("GOSUB command", ExecutionError::NOT_IN_DIRECT_STATE);
    }
    cur++;
    if(cur >= end || cur->type != INTEGER) {
        throw ParseError(tokens, INTEGER, cur - tokens.begin());
    }
    int target = igr(cur++->value);
    if(cur < end && cur->type == COLON) {
        cur++;
    }
    // RETURN continues just after this GOSUB statement.
    state.gosub_stack.push_back({ state.current_line, static_cast<int>(cur - tokens.begin()) });
    state.goto_line = target;
    cur_ = cur;
    return true;
}

// return-statement ::= RETURN (COLON | end)
bool ParseReturnStatement([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != RETURN) {
        return false;
    }
    cur++;
    if(cur < end && cur->type == COLON) {
        cur++;
    }
    if(state.gosub_stack.empty()) {
        throw ExecutionError("RETURN without GOSUB", ExecutionError::NOT_IN_RUN_STATE);
    }
    GosubRecord rec = state.gosub_stack.back();
    state.gosub_stack.pop_back();
    state.goto_line = rec.line;
    state.resume_index = rec.resume_index;
    cur_ = end;
    return true;
}

// on-statement ::= ON numeric-expression (GOTO | GOSUB) integer-list (COLON | end)
bool ParseOnStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != ON) {
        return false;
    }
    if(state.direct) {
        throw ExecutionError("ON command", ExecutionError::NOT_IN_DIRECT_STATE);
    }
    cur++;
    auto sel = ParseNumericExpression(tokens, cur, end, state);
    if(!sel) {
        throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
    }
    bool is_gosub;
    if(ParseOne(tokens, cur, end, state, GOTO)) {
        is_gosub = false;
    } else if(ParseOne(tokens, cur, end, state, GOSUB)) {
        is_gosub = true;
    } else {
        throw ParseError(tokens, GOTO, cur - tokens.begin());
    }
    auto targets = ParseIntegerList(tokens, cur, end);
    if(!targets) {
        throw ParseError(tokens, "integer-list", cur - tokens.begin());
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }

    int k = static_cast<int>(*sel);  // 1-based; out of range falls through
    if(k >= 1 && k <= static_cast<int>(targets->size())) {
        if(is_gosub) {
            state.gosub_stack.push_back({ state.current_line, static_cast<int>(cur - tokens.begin()) });
        }
        state.goto_line = (*targets)[k - 1];
    }
    cur_ = cur;
    return true;
}

// deffn-statement ::= DEF FN NUMBER_IDENTIFIER OPEN_PAREN id {COMMA id} CLOSE_PAREN EQUAL expression (COLON | end)
bool ParseDefStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != DEF) {
        return false;
    }
    cur++;
    if(!ParseOne(tokens, cur, end, state, FN)) {
        throw ParseError(tokens, FN, cur - tokens.begin());
    }
    auto name = ParseIdentifier(tokens, cur, end);
    if(!name) {
        throw ParseError(tokens, "function name", cur - tokens.begin());
    }
    if(!ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
        throw ParseError(tokens, OPEN_PAREN, cur - tokens.begin());
    }
    std::vector<std::string> params;
    do {
        auto p = ParseIdentifier(tokens, cur, end);
        if(!p) {
            throw ParseError(tokens, "parameter", cur - tokens.begin());
        }
        params.push_back(str(*p));
    } while(ParseOne(tokens, cur, end, state, COMMA));
    if(!ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
        throw ParseError(tokens, CLOSE_PAREN, cur - tokens.begin());
    }
    if(!ParseOne(tokens, cur, end, state, EQUAL)) {
        throw ParseError(tokens, EQUAL, cur - tokens.begin());
    }

    // The body is the rest of the statement (an expression up to COLON or line end).
    TokenIterator body_begin = cur;
    while(cur < end && cur->type != COLON) {
        cur++;
    }
    state.functions[str(*name)] = UserFunction{ params, TokenList(body_begin, cur) };
    if(cur < end && cur->type == COLON) {
        cur++;
    }
    cur_ = cur;
    return true;
}

// read-statement ::= READ variable-reference-list (COLON | end)
bool ParseReadStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != READ) {
        return false;
    }
    cur++;
    auto refs = ParseVariableReferenceList(tokens, cur, end, state);
    if(!refs) {
        throw ParseError(tokens, "variable-reference-list", cur - tokens.begin());
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }

    for(const auto& ref : *refs) {
        if(state.data_index >= state.data.size()) {
            throw ExecutionError("out of DATA", ExecutionError::VARIABLE_NOT_FOUND);
        }
        Value v = state.data[state.data_index++];
        bool wants_string = ref.name.back() == '$';
        if(wants_string != is_str(v)) {
            throw ExecutionError(ExecutionError::TYPE_MISMATCH);
        }
        AssignVariable(ref, v, state.variables);
    }
    cur_ = cur;
    return true;
}

// data-statement ::= DATA ...  -- values are gathered at RUN (BuildDataList); skip here
bool ParseDataStatement([[maybe_unused]] const TokenList& tokens, TokenIterator& cur_, TokenIterator end, [[maybe_unused]] State& state)
{
    if(cur_ >= end || cur_->type != DATA) {
        return false;
    }
    cur_ = end;  // the whole rest of the line is data, not statements
    return true;
}

// order-statement ::= ORDER (COLON | end)  -- reset the DATA read pointer (RESTORE)
bool ParseOrderStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    bool succeeded = ParseSingleWordStatement(tokens, cur_, end, state, ORDER);
    if(succeeded) {
        state.data_index = 0;
    }
    return succeeded;
}

// wait-statement ::= WAIT numeric-expression (COLON | end)  -- pause, in milliseconds
bool ParseWaitStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != WAIT) {
        return false;
    }
    cur++;
    auto ms = ParseNumericExpression(tokens, cur, end, state);
    if(!ms) {
        throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }
    // Sleep in short slices so a long WAIT can still be broken out of; the
    // statement is otherwise a single blocking usleep and the program would be
    // unstoppable until it finished.
    constexpr double wait_slice_ms = 50.0;
    for(double remaining = *ms; remaining > 0; remaining -= wait_slice_ms) {
        if(check_break()) {
            throw BreakException{};
        }
        double slice = std::min(remaining, wait_slice_ms);
        usleep(static_cast<useconds_t>(slice * 1000.0));
    }
    cur_ = cur;
    return true;
}

// width-statement ::= WIDTH numeric-expression (COLON | end)
bool ParseWidthStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(cur >= end || cur->type != WIDTH) {
        return false;
    }
    cur++;
    auto w = ParseNumericExpression(tokens, cur, end, state);
    if(!w) {
        throw ParseError(tokens, "numeric-expression", cur - tokens.begin());
    }
    if(cur < end && cur->type == COLON) {
        cur++;
    }
    state.width = static_cast<int>(*w);
    cur_ = cur;
    return true;
}

void ParseStatement(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    // Either this succeeds or throws a parse error
    if(ParseEndStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("end\n");
        return;
    } else if(ParseClearStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("clear\n");
        return;
    } else if(ParseRunStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("run\n");
        return;
    } else if(ParseStopStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("stop\n");
        return;
    } else if(ParseGotoStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("goto %d\n", state.goto_line);
        return;
    } else if(ParsePrintStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("print\n");
        return;
    } else if(ParseLetStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("let\n");
        return;
    } else if(ParseRemStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("rem\n");
        return;
    } else if(ParseDimStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Dim\n");
        return;
    } else if(ParseInputStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("input\n");
        return;
    } else if(ParseIfStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("If\n");
        return;
    } else if(ParseForStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("For\n");
        return;
    } else if(ParseNextStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Next\n");
        return;
    } else if(ParseOnStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("On\n");
        return;
    } else if(ParseGosubStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Gosub\n");
        return;
    } else if(ParseReturnStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Return\n");
        return;
    } else if(ParseWaitStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Wait\n");
        return;
    } else if(ParseWidthStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Width\n");
        return;
    } else if(ParseOrderStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Order\n");
        return;
    } else if(ParseReadStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Read\n");
        return;
    } else if(ParseDataStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Data\n");
        return;
    } else if(ParseDefStatement(tokens, cur_, end, state)) {
        if(debug_statements) printf("Def\n");
        return;
    }
    throw ParseError(tokens, ParseError::UNEXPECTED, cur_ - tokens.begin());
}

// string-expression ::= expression that is a string // returns optional string
std::optional<std::string> ParseStringExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(auto results = ParseExpression(tokens, cur, end, state)) {
        if(is_str(*results)) {
            cur_ = cur;
            return str(*results);
        }
    }
    return {};
}


// integer-expression ::= expression that is an integer // returns optional integer
std::optional<int32_t> ParseIntegerExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(auto results = ParseExpression(tokens, cur, end, state)) {
        if(is_num(*results) && (num(*results) == igr(*results))) {
            cur_ = cur;
            return igr(*results);
        }
    }
    return {};
}

// numeric-expression ::= expression that is a number // returns optional double
std::optional<double> ParseNumericExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;
    if(auto results = ParseExpression(tokens, cur, end, state)) {
        if(is_num(*results)) {
            cur_ = cur;
            return num(*results);
        }
    }
    return {};
}

// variable-reference ::= identifier [OPEN_PAREN integer-expression {COMMA integer-expression} CLOSE_PAREN] // returns VariableReference
std::optional<VariableReference> ParseVariableReference(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    auto cur = cur_;

    auto identifier = ParseIdentifier(tokens, cur, end);
    if(!identifier) {
        return {};
    }

    if(cur >= end || !ParseOne(tokens, cur, end, state, OPEN_PAREN)) {
        cur_ = cur;
        return VariableReference(str(*identifier), {});
    }

    std::vector<int32_t> indices;

    // Subscripts are numeric expressions truncated to integers (BASIC allows e.g.
    // A(X1+X) where X1 came from a division and isn't a whole number).
    auto index = ParseNumericExpression(tokens, cur, end, state);
    if(!index) {
        return {};
    }
    indices.push_back(static_cast<int32_t>(*index));
    while(cur->type == COMMA) {
        cur++;
        index = ParseNumericExpression(tokens, cur, end, state);
        if(!index) {
            return {};
        }
        indices.push_back(static_cast<int32_t>(*index));
    }

    if(cur >= end || !ParseOne(tokens, cur, end, state, CLOSE_PAREN)) {
        return {};
    }

    cur_ = cur;
    return VariableReference(str(*identifier), indices);
}

// statement-list ::= {statement} // returns void
void ParseStatementList(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    while(cur_ < end) {
        // Either this succeeds or throws a parse error
        ParseStatement(tokens, cur_, end, state);

        if(state.goto_line != -1 || state.direct) {
            // A control transfer (GOTO/GOSUB/NEXT/RETURN/ON) or END/STOP happened;
            // the rest of this line is not executed now.  Anything that needs to
            // resume here later (GOSUB/NEXT) already captured its token offset.
            cur_ = end;
            break;
        }
    }
}

// Start ::= statement-list
void Parse(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    // Either this succeeds or throws a parse error
    ParseStatementList(tokens, cur_, end, state);
}

// ** I think these are correct:
// function
// variable-reference
// STRING
// number
// paren-expression ::= OPEN_PAREN expression CLOSE_PAREN // returns Value
// print-statement ::= PRINT {COMMA | SEMICOLON | expression} (COLON | END) // returns void
// unary-operation ::= {unary-op} term // evaluates using unary-ops, returns Value
// term ::= number | STRING | variable-reference | function | paren-expression // returns Value

// ** Not sure about these
// operation ::= expression (POWER | MULTIPLY | DIVIDE | PLUS | MINUS | LESS_THAN | GREATER_THAN | LESS_THAN_EQUAL | GREATER_THAN_EQUAL | EQUAL | NOT_EQUAL | AND | OR) expression // evaluates in correct order, returns Value
// operation is not referenced by any other rule
// expression ::= unary-operation | term // evaluates, returns Value

Value DoOperation(const Value& left, TokenType oper, const Value& right)
{
    // String operands are only valid for PLUS (concatenation) and the relational
    // operators (lexicographic comparison); anything else is a type mismatch.
    if(is_str(left) || is_str(right)) {
        if(!is_str(left) || !is_str(right)) {
            throw ExecutionError(ExecutionError::TYPE_MISMATCH);
        }
        const std::string& l = str(left);
        const std::string& r = str(right);
        switch(oper) {
            case PLUS:               return l + r;
            case EQUAL:              return to_basic_bool(l == r);
            case NOT_EQUAL:          return to_basic_bool(l != r);
            case LESS_THAN:          return to_basic_bool(l < r);
            case GREATER_THAN:       return to_basic_bool(l > r);
            case LESS_THAN_EQUAL:    return to_basic_bool(l <= r);
            case GREATER_THAN_EQUAL: return to_basic_bool(l >= r);
            default:                 throw ExecutionError(ExecutionError::TYPE_MISMATCH);
        }
    }

    double l = num(left);
    double r = num(right);
    switch(oper) {
        case POWER:              return pow(l, r);
        case MULTIPLY:           return l * r;
        case DIVIDE:             return l / r;
        case PLUS:               return l + r;
        case MINUS:              return l - r;
        case LESS_THAN:          return to_basic_bool(l < r);
        case GREATER_THAN:       return to_basic_bool(l > r);
        case LESS_THAN_EQUAL:    return to_basic_bool(l <= r);
        case GREATER_THAN_EQUAL: return to_basic_bool(l >= r);
        case EQUAL:              return to_basic_bool(l == r);
        case NOT_EQUAL:          return to_basic_bool(l != r);
        // AND/OR are bitwise on the truncated integers.  Because BASIC true is -1
        // (all bits set) and false is 0, this gives the usual logical behavior.
        case AND:                return static_cast<double>(static_cast<int32_t>(l) & static_cast<int32_t>(r));
        case OR:                 return static_cast<double>(static_cast<int32_t>(l) | static_cast<int32_t>(r));
        default:
            abort();
            // notreached
            return {0.0};
    }
}

// binary-operator ::= POWER | MULTIPLY | DIVIDE | PLUS | MINUS | LESS_THAN | GREATER_THAN | LESS_THAN_EQUAL | GREATER_THAN_EQUAL | EQUAL | NOT_EQUAL | AND | OR
std::optional<TokenType> ParseBinaryOperator(const TokenList& tokens, TokenIterator& cur_, TokenIterator end);
// expression := unary-operation {binary-operator unary-operation} // evaluate using pair of stacks, return Value
std::optional<Value> ParseExpression(const TokenList& tokens, TokenIterator& cur_, TokenIterator end, State& state)
{
    std::vector<Value> operands;
    std::vector<TokenType> operators;

    auto cur = cur_;

    auto left = ParseUnaryOperation(tokens, cur, end, state);
    if(!left) {
        return {};
    }
    operands.push_back(*left);

    while(auto op = ParseBinaryOperator(tokens, cur, end)) {
        bool did_a_reduce = false;
        while(!operators.empty() && is_higher_precedence(operators.back(), *op)) {
            did_a_reduce = true;

            if(debug_state) { printf("unwinding higher precedence :"); dump_state(operators, operands); puts(""); }
            // printf("%s is higher precedence than %s\n", higher.c_str(), op.c_str());
            TokenType higher = pop(operators);
            Value right = pop(operands); 
            Value left = pop(operands); 
            // printf("unwinding %s is higher precedence than %s\n", TokenTypeToStringMap[higher], TokenTypeToStringMap[*op]);
            // printf("higher precedence reduce : %s %s %s\n", to_str(left).c_str(), TokenTypeToStringMap[higher], to_str(right).c_str());
            operands.push_back(DoOperation(left, higher, right));

            if(did_a_reduce) {
                if(debug_state) { printf("after unwinding higher precedence :"); dump_state(operators, operands); puts(""); }
            }
        }
        operators.push_back(*op);

        auto operand = ParseUnaryOperation(tokens, cur, end, state);
        if(!operand) {
            return {};
        }
        operands.push_back(*operand);
    }

    while(!operators.empty()) {
        if(debug_state) { printf("unwinding final operators :"); dump_state(operators, operands); puts(""); }
        TokenType op = pop(operators);
        Value right = pop(operands); 
        Value left = pop(operands); 
        // printf("finishing lower-precedence operator %s\n", TokenTypeToStringMap[op]);
        // printf("final reduce : %s %s %s\n", to_str(left).c_str(), TokenTypeToStringMap[op], to_str(right).c_str());
        operands.push_back(DoOperation(left, op, right));
    }

    assert(operands.size() == 1);

    cur_ = cur;
    return operands.back();
}

/* 
len-function ::= LEN OPEN_PAREN string-expression CLOSE_PAREN
val-function ::= VAL OPEN_PAREN string-expression CLOSE_PAREN
left-function ::= LEFT OPEN_PAREN string-expression COMMA numeric-expression CLOSE_PAREN
right-function ::= RIGHT OPEN_PAREN string-expression COMMA numeric-expression CLOSE_PAREN
mid-function ::= MID OPEN_PAREN string-expression COMMA numeric-expression [COMMA numeric-expression] CLOSE_PAREN
user-function ::= FN NUMBER_IDENTIFIER OPEN_PAREN numeric-expression [COMMA numeric-expression] CLOSE_PAREN
operation ::= expression (POWER | MULTIPLY | DIVIDE | PLUS | MINUS | LESS_THAN | GREATER_THAN | LESS_THAN_EQUAL | GREATER_THAN_EQUAL | EQUAL | NOT_EQUAL | AND | OR) expression // evaluates in correct order, returns Value
if-statement ::= IF expression THEN (integer | statement) [ELSE (integer | statement)] (COLON | end) // returns void
for-statement ::= FOR NUMBER_IDENTIFIER EQUAL expression TO expression [STEP expression] (COLON | end) // Only number identifiers? (COLON | end) // returns void
next-statement ::= NEXT [NUMBER_IDENTIFIER] (COLON | end) // returns void
on-statement ::= ON integer-expression (GOTO | GOSUB) integer-list (COLON | end) // returns void
gosub-statement ::= GOSUB integer (COLON | end) // returns void
wait-statement ::= WAIT numeric-expression (COLON | end) // returns void
width-statement ::= WIDTH numeric-expression (COLON | end) // returns void
order-statement ::= ORDER INTEGER (COLON | end) // returns void
read-statement ::= READ variable-reference-list (COLON | end) // returns void
deffn-statement ::= DEF FN NUMBER_IDENTIFIER OPEN_PAREN number-identifier-list CLOSE_PAREN numeric-expression (COLON | end) // returns void
no need to do this one: line ::= INTEGER statement-list EOL | statement-list EOL // returns void
return-statement ::= RETURN // returns void

// Needs to be handled in tokenizing and then items between commas used as input
*** expression-list ::= expression {COMMA expression} // returns std::vector<Value>
*** data-statement ::= DATA expression-list (COLON | end) // returns void

std::optional<Token> ParseOptional(const TokenList& tokens, TokenIterator& cur, TokenIterator& end, State& state, const std::set<TokenType>& expect);
std::optional<Token> ParseAny(const TokenList& tokens, TokenIterator& cur, TokenIterator& end, State& state, const std::set<TokenType>& expect);
bool IsOneOf(TokenType type, const std::set<TokenType>& expect);


Parse...
    // If cur is a TokenIterator& with value, the value is cur->value

    // If contents not optional,
    if(cur_ >= end) { return {}; }

    // Make Local Copy
    auto cur = cur_;

    // On Success
    cur_ = cur;
    return thing-for-success;

    // On failure
    return {};

*/


void ParseTest(const TokenList& tokens, TokenIterator& cur_, State& state)
{
    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();
        if(auto ref = ParseVariableReference(tokens, cur, end, state)) {
            printf("variable reference: %s", ref->name.c_str());
            if(!ref->indices.empty()) {
                printf("(%d", static_cast<int>(ref->indices.at(0)));
                for(size_t i = 1; i <  ref->indices.size(); i++) {
                    printf(", %d", static_cast<int>(ref->indices.at(i)));
                }
                printf(")");
            }
            printf("\n");
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();
        if(auto refs = ParseVariableReferenceList(tokens, cur, end, state)) {
            printf("variable references: ");
            for(const auto& ref: *refs) {
                printf("%s", ref.name.c_str());
                if(!ref.indices.empty()) {
                    printf("(%d", static_cast<int>(ref.indices.at(0)));
                    for(size_t i = 1; i <  ref.indices.size(); i++) {
                        printf(", %d", static_cast<int>(ref.indices.at(i)));
                    }
                    printf(")");
                }
                printf(" ");
            }
            printf("\n");
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto token = ParseOne(tokens, cur, end, state, STRING)) {
            assert(is_str(token->value));
            printf("STRING value: %s\n", str(token->value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseIntegerExpression(tokens, cur, end, state)) {
            printf("integer expression: %s\n", std::to_string(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseStringExpression(tokens, cur, end, state)) {
            printf("string expression: %s\n", value->c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseNumericExpression(tokens, cur, end, state)) {
            printf("numeric expression: %s\n", std::to_string(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseParenExpression(tokens, cur, end, state)) {
            printf("Paren expression: %s\n", to_str(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseNumericFunction(tokens, cur, end, state)) {
            printf("function on numeric argument: %s\n", to_str(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseFunction(tokens, cur, end, state)) {
            printf("function: %s\n", to_str(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseExpression(tokens, cur, end, state)) {
            printf("Expression: %s\n", to_str(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseTerm(tokens, cur, end, state)) {
            printf("term: %s\n", to_str(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto value = ParseUnaryOperation(tokens, cur, end, state)) {
            printf("unary operation: %s\n", to_str(*value).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto identifier = ParseIdentifier(tokens, cur, end)) {
            printf("identifier \"%s\"\n", str(*identifier).c_str());
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto number = ParseNumber(tokens, cur, end)) {
            printf("number %f\n", num(*number));
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto integers = ParseIntegerList(tokens, cur, end)) {
            printf("integer list (%lu) ", static_cast<unsigned long>(integers->size()));
            for(auto i: *integers) {
                printf("%d, ", static_cast<int>(i));
            }
            printf("\n");
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto integer = ParseInteger(tokens, cur, end)) {
            printf("integer %d\n", static_cast<int>(igr(*integer)));
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto identifiers = ParseNumberIdentifierList(tokens, cur, end)) {
            printf("identifier list (%lu) ", static_cast<unsigned long>(identifiers->size()));
            for(auto v: *identifiers) {
                printf("%s, ", str(v).c_str());
            }
            printf("\n");
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto ttype = ParseUnaryOp(tokens, cur, end)) {
            printf("unary op %s\n", TokenTypeToStringMap[*ttype]);
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }

    {
        TokenIterator cur = cur_;
        TokenIterator end = tokens.end();

        if(auto ttype = ParseNumericFunctionName(tokens, cur, end)) {
            printf("numeric function name %d %s\n", *ttype, TokenTypeToStringMap[*ttype]);
            printf("    %ld tokens remaining \n", static_cast<long>(end - cur));
        }
    }
}


// Parse and execute one line's worth of tokens, beginning at start_index (used to
// resume a line partway through for FOR/NEXT and GOSUB/RETURN).  Line storage is
// handled by the caller; these tokens never include a leading line number.
void EvaluateTokens(const TokenList& tokens, State& state, int start_index = 0)
{
    TokenIterator cur = tokens.begin() + start_index;
    TokenIterator end = tokens.end();

    if(cur >= end) {
        return;
    }

    /* XXX for bringup */
    if(cur->type == TEST) {
        // XXX special token for testing; skips line number processing
        cur++;
        PrintTokenized(tokens);
        ParseTest(tokens, cur, state);
        exit(0);
    }

    Parse(tokens, cur, end, state);
    if(end - cur > 0) {
        throw ParseError(tokens, ParseError::TRAILING_TOKENS, cur - tokens.begin());
    }

#if 0
    while(line[cur]) {
        double number = -666;

        skip_whitespace();

        if(line[cur] == ';') {
            used = 1;
        } else if(line[cur] == ',') {
            used = 1;
        } else if(line[cur] == '(') {
            operators.push_back("(");
            next_operator_is_unary = true;
            used = 1;
        } else if(line[cur] == ')') {
            bool did_an_unwind = false;
            while(!operators.empty() && operators.back() != "(") {
                if(debug_state) { printf("unwinding to \"(\" :"); dump_state(operators, operands); puts("");}
                did_an_unwind = true;
                std::string op2 = pop(operators);
                // printf("finishing lower-precedence operator %s before \")\"\n", op2.c_str());
                operands.push_back(evaluate(op2, operands));
            }
            if(operators.empty()) {
                printf("unexpected end parenthesis\n");
                abort();
            }
            if(did_an_unwind) {
                if(debug_state) { printf("after unwinding to \"(\" :"); dump_state(operators, operands); puts(""); }
            }
            operators.pop_back();
            if(!operators.empty() && is_function(operators.back())) {
                std::string func = pop(operators);
                // printf("finishing function operator %s before \")\"\n", func.c_str());
                operands.push_back(evaluate(func, operands));
                // printf("after function %s before \")\"\n", func.c_str());
            }
            next_operator_is_unary = false;
            used = 1;
        } else if(auto result = is_operator(line + cur, &used)) {
            auto op = *result;
            if(next_operator_is_unary) {
                if(is_unary_operator(op)) {
                    unary_operators.push_back(op);
                } else {
                    printf("unexpected operator in unary context: \"%s\"\n", op.c_str());
                    abort();
                }
                if(debug_state) { printf("unary operator :"); dump_state(operators, operands); puts(""); }
            } else {
                bool did_an_unwind = false;
                while(!operators.empty() && is_higher_precedence(operators.back(), op)) {
                    did_an_unwind = true;
                    if(debug_state) { printf("unwinding higher precedence :"); dump_state(operators, operands); puts(""); }
                    std::string higher = pop(operators);
                    // printf("%s is higher precedence than %s\n", higher.c_str(), op.c_str());
                    operands.push_back(evaluate(higher, operands));
                }
                if(did_an_unwind) {
                    if(debug_state) { printf("after unwinding higher precedence :"); dump_state(operators, operands); puts(""); }
                }
                operators.push_back(op);
                next_operator_is_unary = is_binary_operator(op);
            }
        } else if(sscanf(line + cur, "\"%[^\"]\"%n", word, &used) == 1) {
            operands.push_back(word);
            if(debug_state) { printf("operand :"); dump_state(operators, operands); puts(""); }
            next_operator_is_unary = false;
        } else if(sscanf(line + cur, "%lf%n", &number, &used) == 1) {
            while(!unary_operators.empty()) {
                std::string op = pop(unary_operators);
                if(op == "-") {
                    number = -number;
                } else if(op == "NOT") {
                    number = -1 - number;
                } else if(op == "+") {
                    // number = number;
                } else {
                    printf("internal error, unary operator \"%s\"\n", op.c_str());
                }
            }
            operands.push_back(number);
            if(debug_state) { printf("operand :"); dump_state(operators, operands); puts(""); }
            next_operator_is_unary = false;
        } else if(sscanf(line + cur, "%[A-Za-z]%n", word, &used) == 1) {
            std::string identifier{str_toupper(word)};
            if(operator_precedence.count(identifier) > 0) {
                operators.push_back(identifier);
                if(debug_state) { printf("operator :"); dump_state(operators, operands); puts(""); }
                next_operator_is_unary = true;
            } else if(state.variables.count(identifier) > 0) {
                operands.push_back(state.variables[identifier]);
                if(debug_state) { printf("variable :"); dump_state(operators, operands); puts(""); }
                next_operator_is_unary = false;
            } else {
                printf("unknown word \"%s\"\n", identifier.c_str());
                abort();
            }
        } else {
            printf("syntax error at \"%s\"\n", line + cur);
            abort();
        }
        assert(used != 0);
        cur += used;
    }
    while(!operators.empty()) {
        std::string op = pop(operators);
        if(debug_state) { printf("unwinding final operators :"); dump_state(operators, operands); puts(""); }
        // printf("finishing lower-precedence operator %s\n", op.c_str());
        operands.push_back(evaluate(op, operands));
    }
    if(debug_state) { printf("final state :"); dump_state(operators, operands); puts(""); }
#if 0
    if(operands.size() > 0) {
        printf("%s\n", to_str(operands.back()).c_str());
    }
    if(operands.size() > 1) {
        printf("extra ");
        dump_operands(operands);
    }
#endif

    if(command == "PRINT") {
        for(auto v: operands) {
            if(is_num(v)) {
                printf("%f ", num(v));
            } else {
                printf("%s ", str(v).c_str());
            }
        }
        printf("\n");
    } else if(command == "LET") {
        auto ref = vref(operands.at(0));
        auto value = operands.at(1);
        state.variables[ref.name] = value;
    } else {
        printf("unimplemented command \"%s\"\n", command.c_str());
    }
#endif
}

void ExecuteNextLine(State& state)
{
    // Poll the console for a BREAK request every 64th line.  A line is the
    // finest granularity at which the interpreter has no partially-executed
    // statement to abandon, but now that lines are not re-tokenized they run in
    // tens of microseconds and the poll's TRAP would be the dominant cost of a
    // simple statement.  64 lines is still well under 100 ms of latency.
    static uint8_t break_poll_counter = 0;
    if((break_poll_counter++ % 64) == 0) {
        if(check_break()) {
            throw BreakException{};
        }
    }

    state.goto_line = -1;

    // Begin where a prior NEXT/RETURN asked us to resume (default: line start),
    // then clear it so the default for the next line is the start.
    int start = state.resume_index;
    state.resume_index = 0;

    // Lines were tokenized when they were entered, so this is just a lookup.
    const TokenList& tokens = state.program.at(state.current_line).tokens;
    EvaluateTokens(tokens, state, start);

    if(state.direct) {
        // END or STOP returned us to direct mode
        return;
    }

    if(state.goto_line == -1) {

        auto next_line = state.program.find(state.current_line);
        next_line++;
        if(next_line == state.program.end()) {
            // Last line of the program
            state.direct = true;
            return;
        }
        state.current_line = next_line->first;

    } else {

        // A GOTO/GOSUB/NEXT/RETURN/ON set a target line; resume_index (if any) was
        // set alongside goto_line for mid-line resume.
        auto next_line = state.program.find(state.goto_line);
        if(next_line == state.program.end()) {
            throw ExecutionError(std::to_string(state.goto_line), ExecutionError::LINE_NOT_FOUND);
        }
        state.current_line = next_line->first;
    }
}

void StopExecution(State& state)
{
    if(!state.direct) {
        printf("Stopped at line %d\n", state.current_line);
    }
    state.direct = true;
}

// If `raw` begins with a line number, split it into that number and the trimmed
// body that follows and return true.  Otherwise return false so the caller
// treats the input as a direct command (or, when loading, ignores it).
bool SplitProgramLine(const std::string& raw, int& line_number, std::string& body)
{
    size_t i = 0;
    while(i < raw.size() && isspace((unsigned char)raw[i])) { i++; }
    if(i >= raw.size() || !isdigit((unsigned char)raw[i])) {
        return false;
    }
    size_t j = i;
    while(j < raw.size() && isdigit((unsigned char)raw[j])) { j++; }
    line_number = std::stoi(raw.substr(i, j - i));

    while(j < raw.size() && isspace((unsigned char)raw[j])) { j++; }
    body = raw.substr(j);
    return true;
}

// Tokenize `body` and store it as program line `line_number`, replacing any line
// with that number.  Throws TokenizeError, leaving the program unchanged, if the
// body does not tokenize; every caller reports that in its own terms.
void StoreProgramLine(int line_number, const std::string& body, State& state)
{
    TokenList tokens = Tokenize(body);
    tokens.shrink_to_fit();  // programs are kept whole; vector slack is not free
    ProgramLine& stored = state.program[line_number];
    stored.source = body;
    stored.tokens = std::move(tokens);
}

// If `raw` begins with a line number, store (or, with an empty body, delete) that
// program line and return true.  A line that does not tokenize is reported here
// and not stored.  Returns false if `raw` is not a numbered line, so the caller
// treats the input as a direct command.
bool MaybeStoreProgramLine(const std::string& raw, State& state)
{
    int line_number;
    std::string body;
    if(!SplitProgramLine(raw, line_number, body)) {
        return false;
    }

    if(body.empty()) {
        state.program.erase(line_number);
    } else {
        try {
            StoreProgramLine(line_number, body, state);
        } catch (const TokenizeError& e) {
            switch(e.type) {
                case TokenizeError::SYNTAX:
                    printf("syntax error at %d (\"%*s\")\n", e.position, std::min(5, (int)(body.size() - e.position)), body.c_str() + e.position);
                    break;
            }
        }
    }
    return true;
}

// Split off the first whitespace-delimited word (uppercased) from `line`, leaving
// the trimmed remainder in `arg`.  Used to recognize the direct-mode commands.
std::string FirstWord(const std::string& line, std::string& arg)
{
    size_t i = 0;
    while(i < line.size() && isspace((unsigned char)line[i])) { i++; }
    size_t j = i;
    while(j < line.size() && !isspace((unsigned char)line[j])) { j++; }
    std::string word = str_toupper(line.substr(i, j - i));

    size_t k = j;
    while(k < line.size() && isspace((unsigned char)line[k])) { k++; }
    size_t e = line.find_last_not_of(" \t");
    arg = (k > e || e == std::string::npos) ? "" : line.substr(k, e - k + 1);
    return word;
}

// Remove a single pair of surrounding double quotes, if present.
std::string StripQuotes(const std::string& s)
{
    if(s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// LOAD: replace the stored program with the numbered lines of a file.  Non-numbered
// lines (e.g. a trailing RUN) are ignored.  A line that does not tokenize is
// reported and skipped and the rest of the file still loads; nothing is thrown,
// because the batch path loads outside the interpreter's error handling.
// Returns false if the file can't be read.
bool LoadProgram(const std::string& filename, State& state)
{
    std::ifstream in(filename);
    if(!in) {
        printf("Could not open \"%s\"\n", filename.c_str());
        return false;
    }
    state.program.clear();
    std::string line;
    while(std::getline(in, line)) {
        if(!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        int line_number;
        std::string body;
        if(!SplitProgramLine(line, line_number, body)) {
            continue;
        }
        if(body.empty()) {
            state.program.erase(line_number);
            continue;
        }
        try {
            StoreProgramLine(line_number, body, state);
        } catch (const TokenizeError& e) {
            printf("syntax error in line %d\n", line_number);
        }
    }
    return true;
}

// SAVE: write the stored program as "<n> <source>" lines.
bool SaveProgram(const std::string& filename, State& state)
{
    std::ofstream out(filename);
    if(!out) {
        printf("Could not write \"%s\"\n", filename.c_str());
        return false;
    }
    for(const auto& [n, stored] : state.program) {
        out << n << " " << stored.source << "\n";
    }
    return true;
}

void ListProgram(State& state)
{
    for(const auto& [n, stored] : state.program) {
        printf("%d %s\n", n, stored.source.c_str());
    }
}

// FILES: list a directory (the CF card root by default).
//
// This uses griffin_readdir() rather than the POSIX opendir()/readdir() veneer
// because the veneer's struct dirent carries no file size and the firmware's
// stat() is a stub; the Griffin call returns name, size and directory flag in
// one go.
void ListFiles(const std::string& path)
{
    const std::string dir = path.empty() ? std::string("/") : path;

    for(int index = 0; ; index++) {
        GriffinDirEnt entry;
        int result = griffin_readdir(dir.c_str(), index, &entry);
        if(result < 0) {
            printf("Could not read directory \"%s\"\n", dir.c_str());
            return;
        }
        if(result > 0) {
            return;  // past the last entry
        }
        if(entry.is_dir) {
            printf("%-14s <DIR>\n", entry.name);
        } else {
            printf("%-14s %lu\n", entry.name, static_cast<unsigned long>(entry.size));
        }
    }
}

void NewProgram(State& state)
{
    state.program.clear();
    state.variables.clear();
    state.for_stack.clear();
    state.gosub_stack.clear();
    state.functions.clear();
    state.data.clear();
    state.data_index = 0;
}

int main(int argc, char **argv)
{
    State state;
    bool go = true;

    // Nothing else in the program seeds the generator, so without this RND
    // would replay the same sequence on every run.
    srand48(static_cast<long>(griffin_getticks()));

    printf("Griffin BASIC.  LOAD/SAVE/LIST/FILES/RUN; QUIT to exit.\n");

    // With a program file on the command line, load and run it, then exit.
    bool batch = false;
    if(argc > 1) {
        if(!LoadProgram(argv[1], state)) {
            return 1;
        }
        StartRun(state);
        batch = true;
    }

    while(go) {
        try {
            if(state.direct) {
                if(batch) {
                    // The command-line program has finished (or stopped on error).
                    go = false;
                    continue;
                }
                static char buf[512];
                if(fgets(buf, sizeof(buf), stdin) != nullptr) {
                    std::string line(buf);
                    if(!line.empty() && line.back() == '\n') {
                        line.pop_back();
                    }

                    std::string arg;
                    std::string cmd = FirstWord(line, arg);

                    if(cmd == "QUIT" || cmd == "BYE") {
                        go = false;
                    } else if(cmd == "NEW") {
                        NewProgram(state);
                    } else if(cmd == "LIST") {
                        ListProgram(state);
                    } else if(cmd == "FILES") {
                        ListFiles(StripQuotes(arg));
                    } else if(cmd == "LOAD") {
                        LoadProgram(StripQuotes(arg), state);
                    } else if(cmd == "SAVE") {
                        SaveProgram(StripQuotes(arg), state);
                    } else if(cmd == "RUN" && !arg.empty()) {
                        if(LoadProgram(StripQuotes(arg), state)) {
                            StartRun(state);
                        }
                    } else if(MaybeStoreProgramLine(line, state)) {
                        // numbered line stored as source; nothing to run now
                    } else {
                        try {
                            TokenList tokens = Tokenize(line);
                            EvaluateTokens(tokens, state);
                        } catch (const TokenizeError& e) {
                            switch(e.type) {
                                case TokenizeError::SYNTAX:
                                    printf("syntax error at %d (\"%*s\")\n", e.position, std::min(5, (int)(line.size() - e.position)), line.c_str() + e.position);
                                    break;
                            }
                        }
                    }
                } else {
                    go = false;
                }
            } else {
                ExecuteNextLine(state);
            }
        } catch (const BreakException&) {
            if(state.direct) {
                printf("BREAK\n");
            } else {
                printf("BREAK IN %d\n", state.current_line);
            }
            state.direct = true;
        } catch (const ParseError& e) {
            switch(e.type) {
                case ParseError::TRAILING_TOKENS:
                    printf("unrecognized trailing tokens\n");
                    break;
                case ParseError::UNEXPECTED_END:
                    printf("unexpected end of tokens while parsing\n");
                    break;
                case ParseError::UNEXPECTED:
                    printf("unexpected token while parsing\n");
                    break;
                case ParseError::EXPECTED_TOKEN:
                    printf("expected %s token at %d while parsing\n", TokenTypeToStringMap[e.expected_token_type], e.token);
                    break;
                case ParseError::EXPECTED_RULE:
                    printf("expected %s at %d while parsing\n", TokenTypeToStringMap[e.expected_token_type], e.token);
                    break;
            }
            PrintTokenized(e.tokens, e.token);
            StopExecution(state);
        } catch (const ExecutionError& e) {
            switch(e.type) {
                case ExecutionError::NOT_IN_RUN_STATE:
                    printf("RUN state required; %s\n", e.why.c_str());
                    break;
                case ExecutionError::NOT_IN_DIRECT_STATE:
                    printf("DIRECT state required; %s\n", e.why.c_str());
                    break;
                case ExecutionError::LINE_NOT_FOUND:
                    printf("Line %s not found\n", e.why.c_str());
                    break;
                case ExecutionError::VARIABLE_NOT_FOUND:
                    printf("Variable %s not found\n", e.why.c_str());
                    break;
                case ExecutionError::TYPE_MISMATCH:
                    printf("Type mismatch\n");
                    break;
            }
            StopExecution(state);
        } catch (const VariableDimensionError& e) {
            printf("array access for variable \"%s\" used %d dimensions, expected %d\n", e.var.c_str(), e.used, e.expected);
            StopExecution(state);
        } catch (const VariableReferenceBoundsError& e) {
            printf("variable \"%s\" referenced %d out of array bounds %d\n", e.var.c_str(),
                static_cast<int>(e.index), static_cast<int>(e.size));
            StopExecution(state);
        } catch (const TokenizeError& e) {
            printf("syntax error at %d while tokenizing line %d\n", e.position, state.current_line);
            StopExecution(state);
        } catch (const std::bad_alloc&) {
            // DIM of an oversized array is the way a program runs the app heap
            // out; report it and drop back to direct mode rather than letting
            // it reach terminate().
            printf("Out of memory\n");
            StopExecution(state);
        }
    }
}
