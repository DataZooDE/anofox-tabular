#pragma once

#include <string>
#include <unordered_set>

namespace duckdb {
namespace anofox {

/**
 * Common given names for the dictionary fallback of NAME detection (used when
 * the NER model is not available or finds no person in a text): frequent
 * US/UK, Western European and German/Austrian/Swiss first names, lowercase
 * UTF-8 ("jürgen", "bärbel"). Given names that are also frequent words or
 * abbreviations in business text ("Jan" for January, "Ernst", "Roman") are
 * left out on purpose.
 */
const std::unordered_set<std::string> &CommonGivenNames();

} // namespace anofox
} // namespace duckdb
