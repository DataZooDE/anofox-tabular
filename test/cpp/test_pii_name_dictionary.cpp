// Unit tests for the dictionary fallback of NAME detection, which runs when
// the NER model is not available (offline, no OpenVINO) or finds no person.
// The SQL test anofox_pii_ner_offline.test covers the same behaviour end to
// end but needs a process without the model; these tests always run.
//
// Linked into DuckDB's Catch2 `unittest` runner; test names start with
// "test/cpp/" so the default "test/*" filter picks them up.

#include "catch.hpp"

#include "anofox_pii.hpp"

#include <string>
#include <vector>

using duckdb::anofox::NameRecognizer;
using duckdb::anofox::PIIMatch;

namespace {

std::vector<std::string> FallbackNames(const std::string &text) {
    std::vector<std::string> names;
    for (const auto &match : NameRecognizer::ExtractWithDictionary(text)) {
        REQUIRE(text.substr(match.start_pos, match.end_pos - match.start_pos) == match.matched_text);
        REQUIRE(match.confidence == 0.5);
        names.push_back(match.matched_text);
    }
    return names;
}

} // namespace

TEST_CASE("test/cpp/anofox_pii_name_dictionary_dach", "[anofox_pii]") {
    // German given names, umlauts and ß in given names and surnames
    REQUIRE(FallbackNames("Joachim Sauer, Sabine Weiß, Peter Müller") ==
            std::vector<std::string> {"Joachim Sauer", "Sabine Weiß", "Peter Müller"});
    REQUIRE(FallbackNames("Rückfrage von Jürgen Schäfer und Björn Großmann an Klaus-Peter Öztürk") ==
            std::vector<std::string> {"Jürgen Schäfer", "Björn Großmann", "Klaus-Peter Öztürk"});
    // Austrian and Swiss given names; a name particle joins the surname
    REQUIRE(FallbackNames("Grüezi, hier ist Urs Brändli aus Zürich, und Ursula von der Leyen.") ==
            std::vector<std::string> {"Urs Brändli", "Ursula von der Leyen"});
    REQUIRE(FallbackNames("Servus, Leopold Hütter und Notburga Ölz") ==
            std::vector<std::string> {"Leopold Hütter", "Notburga Ölz"});
    // A given name alone, and at most two following capitalised words
    REQUIRE(FallbackNames("Kundin: Bärbel") == std::vector<std::string> {"Bärbel"});
    REQUIRE(FallbackNames("Anna Maria Huber, Abteilung Einkauf") == std::vector<std::string> {"Anna Maria Huber"});
}

TEST_CASE("test/cpp/anofox_pii_name_dictionary_whole_words", "[anofox_pii]") {
    // No match inside a longer word, for lowercase words or all-caps text
    REQUIRE(FallbackNames("Annabelle bestellt Sabinenkraut").empty());
    REQUIRE(FallbackNames("peter müller schreibt klein").empty());
    REQUIRE(FallbackNames("PETER MÜLLER").empty());
    // A digit makes a token no word
    REQUIRE(FallbackNames("Login Peter2024 aktiv").empty());
    // Words that are not separated by a single space do not extend the name
    REQUIRE(FallbackNames("Sabine;Weiß") == std::vector<std::string> {"Sabine"});
    // Common words and month abbreviations are not in the dictionary
    REQUIRE(FallbackNames("Lieferung am 15. Jan 2024, Ernst der Lage").empty());
    // Byte offsets point into the UTF-8 text
    const std::string text = "Grüße an Jürgen";
    const auto matches = NameRecognizer::ExtractWithDictionary(text);
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0].start_pos == std::string("Grüße an ").size());
    REQUIRE(matches[0].end_pos == text.size());
}
