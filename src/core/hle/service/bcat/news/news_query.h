// SPDX-FileCopyrightText: 2026 tobagin
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <functional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "core/hle/service/bcat/news/news_record.h"

namespace Service::News {

// A WHERE or ORDER BY string outside the supported grammar.
class QueryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The WHERE and ORDER BY strings INewsDatabaseService receives. The console hands them to SQLite with a
// few functions of its own; qlaunch 22.5.0 sends only integer and string literals, columns,
// comparisons, AND/OR, parentheses, `*`, ASC/DESC and the functions N_IF, N_SWITCH, N_RAND and
// N_PMATCH. That subset is evaluated here; anything else throws QueryError. Values follow SQLite:
// integers and text, integers ordering before text, text that is not a number counting as 0.
// Behaviour follows the Ryujinx Harbor (MIT-licensed Ryujinx base).
std::function<bool(const NewsRow&)> ParseWhere(std::string_view text);

// The rows in the order of `text` (stable; empty text keeps the order given).
std::vector<NewsRow> OrderRows(std::vector<NewsRow> rows, std::string_view text);

} // namespace Service::News
