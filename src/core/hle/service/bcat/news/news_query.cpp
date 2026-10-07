// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#include "core/hle/service/bcat/news/news_query.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <memory>
#include <random>

// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).

namespace Service::News {
namespace {

using Eval = std::function<NewsValue(const NewsRow&)>;

bool IEquals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) ==
                      std::toupper(static_cast<unsigned char>(y));
           });
}

bool IsLetter(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}
bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

int Compare(const NewsValue& a, const NewsValue& b) {
    if (std::holds_alternative<s64>(a) && std::holds_alternative<s64>(b)) {
        const s64 x = std::get<s64>(a);
        const s64 y = std::get<s64>(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (std::holds_alternative<std::string>(a) && std::holds_alternative<std::string>(b)) {
        const int order = std::get<std::string>(a).compare(std::get<std::string>(b));
        return order < 0 ? -1 : (order > 0 ? 1 : 0);
    }
    // An integer orders before text.
    return std::holds_alternative<s64>(a) ? -1 : 1;
}

s64 ToInteger(const NewsValue& value) {
    if (std::holds_alternative<s64>(value)) {
        return std::get<s64>(value);
    }
    const auto& text = std::get<std::string>(value);
    s64 number{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    return error == std::errc{} && end == text.data() + text.size() ? number : 0;
}

bool Truth(const NewsValue& value) {
    return ToInteger(value) != 0;
}

NewsValue Bool(bool value) {
    return static_cast<s64>(value ? 1 : 0);
}

struct Token {
    enum class Kind { Word, Number, String, Symbol } kind;
    std::string text;
};

std::vector<Token> Tokenize(std::string_view text) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const size_t start = i;
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (IsLetter(c) || c == '_') {
            while (i < text.size() && (IsLetter(text[i]) || IsDigit(text[i]) || text[i] == '_')) {
                ++i;
            }
            tokens.push_back({Token::Kind::Word, std::string(text.substr(start, i - start))});
        } else if (IsDigit(c)) {
            while (i < text.size() && IsDigit(text[i])) {
                ++i;
            }
            tokens.push_back({Token::Kind::Number, std::string(text.substr(start, i - start))});
        } else if (c == '\'') {
            // '' is a quote inside the string.
            std::string literal;
            for (++i;; ++i) {
                if (i >= text.size()) {
                    throw QueryError("unterminated string");
                }
                if (text[i] == '\'') {
                    if (i + 1 < text.size() && text[i + 1] == '\'') {
                        literal.push_back('\'');
                        ++i;
                        continue;
                    }
                    ++i;
                    break;
                }
                literal.push_back(text[i]);
            }
            tokens.push_back({Token::Kind::String, std::move(literal)});
        } else if (i + 1 < text.size() &&
                   (text.substr(i, 2) == "!=" || text.substr(i, 2) == "<>" ||
                    text.substr(i, 2) == "<=" || text.substr(i, 2) == ">=" ||
                    text.substr(i, 2) == "==")) {
            tokens.push_back({Token::Kind::Symbol, std::string(text.substr(i, 2))});
            i += 2;
        } else if (std::string_view("=<>*(),").find(c) != std::string_view::npos) {
            tokens.push_back({Token::Kind::Symbol, std::string(1, c)});
            ++i;
        } else {
            throw QueryError(std::string("unexpected '") + c + "'");
        }
    }
    return tokens;
}

class Parser {
public:
    explicit Parser(std::string_view text) : tokens(Tokenize(text)) {}

    const Token* Peek() const {
        return next < tokens.size() ? &tokens[next] : nullptr;
    }

    bool Accept(std::string_view word) {
        const Token* token = Peek();
        if (token != nullptr && token->kind != Token::Kind::String && IEquals(token->text, word)) {
            ++next;
            return true;
        }
        return false;
    }

    void Expect(std::string_view word) {
        if (!Accept(word)) {
            throw QueryError("expected '" + std::string(word) + "'");
        }
    }

    void End() const {
        if (Peek() != nullptr) {
            throw QueryError("unexpected '" + Peek()->text + "'");
        }
    }

    Eval Or() {
        Eval left = And();
        while (Accept("OR")) {
            Eval a = std::move(left);
            Eval b = And();
            left = [a, b](const NewsRow& row) { return Bool(Truth(a(row)) || Truth(b(row))); };
        }
        return left;
    }

private:
    Eval And() {
        Eval left = Comparison();
        while (Accept("AND")) {
            Eval a = std::move(left);
            Eval b = Comparison();
            left = [a, b](const NewsRow& row) { return Bool(Truth(a(row)) && Truth(b(row))); };
        }
        return left;
    }

    Eval Comparison() {
        Eval left = Product();
        const Token* token = Peek();
        if (token == nullptr || token->kind != Token::Kind::Symbol ||
            (token->text != "=" && token->text != "==" && token->text != "!=" &&
             token->text != "<>" && token->text != "<" && token->text != "<=" &&
             token->text != ">" && token->text != ">=")) {
            return left;
        }
        const std::string op = token->text;
        ++next;
        Eval right = Product();
        return [left, right, op](const NewsRow& row) {
            const int order = Compare(left(row), right(row));
            bool result;
            if (op == "=" || op == "==") {
                result = order == 0;
            } else if (op == "!=" || op == "<>") {
                result = order != 0;
            } else if (op == "<") {
                result = order < 0;
            } else if (op == "<=") {
                result = order <= 0;
            } else if (op == ">") {
                result = order > 0;
            } else {
                result = order >= 0;
            }
            return Bool(result);
        };
    }

    Eval Product() {
        Eval left = Primary();
        while (Accept("*")) {
            Eval a = std::move(left);
            Eval b = Primary();
            left = [a, b](const NewsRow& row) -> NewsValue {
                return ToInteger(a(row)) * ToInteger(b(row));
            };
        }
        return left;
    }

    Eval Primary() {
        if (Peek() == nullptr) {
            throw QueryError("unexpected end");
        }
        const Token token = *Peek();
        ++next;

        switch (token.kind) {
        case Token::Kind::String: {
            return [text = token.text](const NewsRow&) -> NewsValue { return text; };
        }
        case Token::Kind::Number: {
            s64 number{};
            const auto [end, error] = std::from_chars(
                token.text.data(), token.text.data() + token.text.size(), number);
            if (error != std::errc{}) {
                throw QueryError("integer '" + token.text + "' out of range");
            }
            return [number](const NewsRow&) -> NewsValue { return number; };
        }
        case Token::Kind::Symbol: {
            if (token.text != "(") {
                throw QueryError("unexpected '" + token.text + "'");
            }
            Eval inner = Or();
            Expect(")");
            return inner;
        }
        case Token::Kind::Word:
            break;
        }

        if (Accept("(")) {
            std::string name = token.text;
            std::transform(name.begin(), name.end(), name.begin(),
                           [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            return Function(name);
        }

        std::string column = token.text;
        std::transform(column.begin(), column.end(), column.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (!NewsRow::IsColumn(column)) {
            throw QueryError("unknown column '" + token.text + "'");
        }
        return [column](const NewsRow& row) -> NewsValue { return *row.Get(column); };
    }

    Eval Function(const std::string& name) {
        std::vector<Eval> args;
        if (!Accept(")")) {
            do {
                args.push_back(Or());
            } while (Accept(","));
            Expect(")");
        }

        // The semantics are read from how qlaunch uses them: N_SWITCH(x, k1, v1, ..., default) picks
        // the value of the first key equal to x (its `displayed` update ends "0)=1"), N_IF(c, a, b)
        // is a conditional. N_PMATCH is a substring test, from qlaunch's '/%016llx/' pattern against
        // application_ids.
        if (name == "N_IF" && args.size() == 3) {
            return [args](const NewsRow& row) {
                return Truth(args[0](row)) ? args[1](row) : args[2](row);
            };
        }
        if (name == "N_SWITCH" && args.size() >= 2 && args.size() % 2 == 0) {
            return [args](const NewsRow& row) {
                const NewsValue subject = args[0](row);
                for (size_t i = 1; i + 1 < args.size(); i += 2) {
                    if (Compare(subject, args[i](row)) == 0) {
                        return args[i + 1](row);
                    }
                }
                return args.back()(row);
            };
        }
        if (name == "N_RAND" && args.size() == 2) {
            return [args](const NewsRow& row) -> NewsValue {
                static thread_local std::mt19937_64 engine{std::random_device{}()};
                const s64 low = ToInteger(args[0](row));
                const s64 high = (std::max)(low, ToInteger(args[1](row)));
                return std::uniform_int_distribution<s64>(low, high)(engine);
            };
        }
        if (name == "N_PMATCH" && args.size() == 2) {
            return [args](const NewsRow& row) {
                const NewsValue text = args[0](row);
                const NewsValue pattern = args[1](row);
                return Bool(std::holds_alternative<std::string>(text) &&
                            std::holds_alternative<std::string>(pattern) &&
                            std::get<std::string>(text).find(std::get<std::string>(pattern)) !=
                                std::string::npos);
            };
        }
        throw QueryError("unknown function " + name + " with " + std::to_string(args.size()) +
                         " argument(s)");
    }

    std::vector<Token> tokens;
    size_t next{};
};

bool IsBlank(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

std::function<bool(const NewsRow&)> ParseWhere(std::string_view text) {
    if (IsBlank(text)) {
        return [](const NewsRow&) { return true; };
    }

    Parser parser{text};
    Eval condition = parser.Or();
    parser.End();

    return [condition](const NewsRow& row) { return Truth(condition(row)); };
}

std::vector<NewsRow> OrderRows(std::vector<NewsRow> rows, std::string_view text) {
    if (IsBlank(text)) {
        return rows;
    }

    Parser parser{text};
    std::vector<std::pair<Eval, bool>> keys;
    do {
        Eval key = parser.Or();
        const bool descending = parser.Accept("DESC");
        if (!descending) {
            parser.Accept("ASC");
        }
        keys.emplace_back(std::move(key), descending);
    } while (parser.Accept(","));
    parser.End();

    // Keys are evaluated once per row, so N_RAND gives a row one value for the whole sort.
    struct Entry {
        NewsRow row;
        std::vector<NewsValue> values;
    };
    std::vector<Entry> entries;
    entries.reserve(rows.size());
    for (auto& row : rows) {
        Entry entry;
        for (const auto& key : keys) {
            entry.values.push_back(key.first(row));
        }
        entry.row = std::move(row);
        entries.push_back(std::move(entry));
    }

    std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        for (size_t i = 0; i < keys.size(); ++i) {
            const int order = Compare(a.values[i], b.values[i]);
            if (order != 0) {
                return keys[i].second ? order > 0 : order < 0;
            }
        }
        return false;
    });

    std::vector<NewsRow> sorted;
    sorted.reserve(entries.size());
    for (auto& entry : entries) {
        sorted.push_back(std::move(entry.row));
    }
    return sorted;
}

} // namespace Service::News
