//===----------------------------------------------------------------------===//
// Catch2 unit tests for NER input bounding (issue #56).
//
// Covers the header-only helpers used by NERModelManager::RunSingleInference:
//  - PlanNERWindows / SliceTokenizedWindow / CombineWindowEntities: split
//    inputs longer than the model's maximum sequence length into overlapping
//    windows and combine their entities
//  - HasNaturalLanguageWord: which values NER skips
//  - ValidateLogitsShape: rejects output tensors whose shape does not match
//    seq_length x NUM_LABELS before any logits are read
//  - BasicWordSplit: the BERT-style (Unicode-aware) word splitting of
//    the hand-written WordPiece tokenizer (UTF-8 sequences stay intact inside
//    words; Unicode punctuation, spaces and CJK ideographs split words)
//
// Built into anofox_tabular_cpp_tests; no OpenVINO runtime required.
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "anofox_ner.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using duckdb::anofox::BasicWordSplit;
using duckdb::anofox::CombineWindowEntities;
using duckdb::anofox::HasNaturalLanguageWord;
using duckdb::anofox::NER_WINDOW_OVERLAP;
using duckdb::anofox::NEREntity;
using duckdb::anofox::NERWindow;
using duckdb::anofox::PlanNERWindows;
using duckdb::anofox::SliceTokenizedWindow;
using duckdb::anofox::TokenizedInput;
using duckdb::anofox::ValidateLogitsShape;

namespace {

constexpr int64_t SEP_ID = 102;

} // namespace

TEST_CASE("PlanNERWindows keeps inputs that fit into one window", "[ner][windows]") {
    auto windows = PlanNERWindows(100, 254, NER_WINDOW_OVERLAP);
    REQUIRE(windows.size() == 1);
    REQUIRE(windows[0].begin == 0);
    REQUIRE(windows[0].end == 100);
    REQUIRE(windows[0].own_begin == 0);
    REQUIRE(windows[0].own_end == 100);

    // Exactly one window's worth, and "unbounded"
    REQUIRE(PlanNERWindows(254, 254, NER_WINDOW_OVERLAP).size() == 1);
    REQUIRE(PlanNERWindows(5000, 0, NER_WINDOW_OVERLAP).size() == 1);
}

TEST_CASE("PlanNERWindows covers long inputs with overlapping windows", "[ner][windows]") {
    for (size_t content : {255, 866, 1200, 5000}) {
        auto windows = PlanNERWindows(content, 254, NER_WINDOW_OVERLAP);
        REQUIRE(windows.size() >= 2);
        REQUIRE(windows.front().begin == 0);
        REQUIRE(windows.back().end == content);
        size_t expected_own_begin = 0;
        for (size_t i = 0; i < windows.size(); ++i) {
            const auto &w = windows[i];
            // Model-sized windows
            REQUIRE(w.end - w.begin == 254);
            // Owned ranges partition the input and lie inside the window
            REQUIRE(w.own_begin == expected_own_begin);
            REQUIRE(w.own_begin < w.own_end);
            REQUIRE(w.own_begin >= w.begin);
            REQUIRE(w.own_end <= w.end);
            expected_own_begin = w.own_end;
            if (i > 0) {
                // Neighbours overlap by at least the configured overlap, and
                // each window keeps half of it as context on either side
                REQUIRE(windows[i - 1].end >= w.begin + NER_WINDOW_OVERLAP);
                REQUIRE(w.own_begin - w.begin >= NER_WINDOW_OVERLAP / 2);
                REQUIRE(windows[i - 1].end - windows[i - 1].own_end >= NER_WINDOW_OVERLAP / 2);
            }
        }
        REQUIRE(expected_own_begin == content);
    }
}

TEST_CASE("SliceTokenizedWindow keeps special tokens and original offsets", "[ner][windows]") {
    // [CLS] t0 .. t29 [SEP] with offsets into a 7-byte-per-token UTF-8 text
    TokenizedInput full;
    full.ids.push_back(101);
    full.offsets.push_back({0, 0});
    for (size_t i = 0; i < 30; ++i) {
        full.ids.push_back(static_cast<int64_t>(2000 + i));
        full.offsets.push_back({i * 7, i * 7 + 6});
    }
    full.ids.push_back(SEP_ID);
    full.offsets.push_back({210, 210});

    NERWindow window {10, 20, 12, 18};
    auto slice = SliceTokenizedWindow(full, window);
    REQUIRE(slice.ids.size() == 12);
    REQUIRE(slice.offsets.size() == 12);
    REQUIRE(slice.ids.front() == 101);
    REQUIRE(slice.ids.back() == SEP_ID);
    for (size_t i = 0; i < 10; ++i) {
        REQUIRE(slice.ids[i + 1] == full.ids[window.begin + i + 1]);
        REQUIRE(slice.offsets[i + 1] == full.offsets[window.begin + i + 1]);
    }
    // The trailing special token has an empty range
    REQUIRE(slice.offsets.back().first == slice.offsets.back().second);
}

TEST_CASE("CombineWindowEntities removes duplicates from overlapping windows", "[ner][windows]") {
    const std::string text = "Hello Jennifer Wilson from Acme Corp in Berlin";
    // The same name seen by two windows, once cut short at a window edge
    std::vector<NEREntity> entities = {
        NEREntity("Jennifer Wilson", "PER", 6, 21, 0.97),
        NEREntity("Jennifer", "PER", 6, 14, 0.80),
        NEREntity("Acme Corp", "ORG", 27, 36, 0.90),
        NEREntity("Acme", "LOC", 27, 31, 0.75),
        NEREntity("Berlin", "LOC", 40, 46, 0.99),
        NEREntity("Berlin", "LOC", 40, 46, 0.98),
    };
    auto combined = CombineWindowEntities(entities, text);
    REQUIRE(combined.size() == 3);
    REQUIRE(combined[0].text == "Jennifer Wilson");
    REQUIRE(combined[0].confidence == 0.97);
    REQUIRE(combined[1].text == "Acme Corp");
    REQUIRE(combined[1].label == "ORG");
    REQUIRE(combined[2].text == "Berlin");

    // Partial spans of one type are united to the outermost span
    auto united = CombineWindowEntities({NEREntity("Jennifer", "PER", 6, 14, 0.9),
                                         NEREntity("Jennifer Wilson", "PER", 6, 21, 0.8),
                                         NEREntity("Wilson", "PER", 15, 21, 0.95)},
                                        text);
    REQUIRE(united.size() == 1);
    REQUIRE(united[0].text == "Jennifer Wilson");
    REQUIRE(united[0].start_pos == 6);
    REQUIRE(united[0].end_pos == 21);
}

TEST_CASE("HasNaturalLanguageWord tells text from technical values", "[ner][skip]") {
    // Natural language and names
    REQUIRE(HasNaturalLanguageWord("Anna Schmidt"));
    REQUIRE(HasNaturalLanguageWord("Li"));
    REQUIRE(HasNaturalLanguageWord("J. Smith"));
    REQUIRE(HasNaturalLanguageWord("M\xC3\xBCller"));
    REQUIRE(HasNaturalLanguageWord("order 4711 shipped"));
    REQUIRE(HasNaturalLanguageWord("ID: 12345"));
    // No word of two or more letters
    REQUIRE_FALSE(HasNaturalLanguageWord(""));
    REQUIRE_FALSE(HasNaturalLanguageWord("d41d8cd98f00b204e9800998ecf8427e"));
    REQUIRE_FALSE(HasNaturalLanguageWord("550e8400-e29b-41d4-a716-446655440000"));
    REQUIRE_FALSE(HasNaturalLanguageWord("4711.00"));
    REQUIRE_FALSE(HasNaturalLanguageWord("+49 171 1234567"));
    REQUIRE_FALSE(HasNaturalLanguageWord("A-12 / B-7"));
    REQUIRE_FALSE(HasNaturalLanguageWord("Zm9vYmFy0x9"));
}

TEST_CASE("ValidateLogitsShape accepts matching shapes", "[ner][bounding]") {
    REQUIRE_NOTHROW(ValidateLogitsShape({1, 16, 9}, 16, 9));
    // Rank-2 outputs with the same element count are also acceptable
    REQUIRE_NOTHROW(ValidateLogitsShape({16, 9}, 16, 9));
}

TEST_CASE("ValidateLogitsShape rejects mismatched shapes", "[ner][bounding]") {
    // Wrong sequence length
    REQUIRE_THROWS_AS(ValidateLogitsShape({1, 8, 9}, 16, 9), std::runtime_error);
    // Wrong label count
    REQUIRE_THROWS_AS(ValidateLogitsShape({1, 16, 10}, 16, 9), std::runtime_error);
    // Empty shape
    REQUIRE_THROWS_AS(ValidateLogitsShape({}, 16, 9), std::runtime_error);
    // Unexpected batch dimension
    REQUIRE_THROWS_AS(ValidateLogitsShape({2, 16, 9}, 16, 9), std::runtime_error);

    // Error message names the actual and expected shape
    try {
        ValidateLogitsShape({1, 8, 9}, 16, 9);
        FAIL("expected std::runtime_error");
    } catch (const std::runtime_error &e) {
        std::string message = e.what();
        REQUIRE(message.find("shape") != std::string::npos);
        REQUIRE(message.find("16") != std::string::npos);
    }
}

TEST_CASE("BasicWordSplit splits ASCII words and punctuation", "[ner][tokenizer]") {
    auto words = BasicWordSplit("John Smith, CEO of ACME Inc.");
    std::vector<std::string> expected = {"John", "Smith", ",", "CEO", "of", "ACME", "Inc", "."};
    REQUIRE(words == expected);
}

TEST_CASE("BasicWordSplit keeps UTF-8 multibyte sequences intact", "[ner][tokenizer][utf8]") {
    auto words = BasicWordSplit("Herr Müller wohnt in Köln");
    std::vector<std::string> expected = {"Herr", "M\xC3\xBCller", "wohnt", "in", "K\xC3\xB6ln"};
    REQUIRE(words == expected);

    // A broken UTF-8 sequence stays in its word; no byte is dropped
    auto broken = BasicWordSplit("ab\xC3" "x \xFF");
    std::vector<std::string> expected_broken = {"ab\xC3" "x", "\xFF"};
    REQUIRE(broken == expected_broken);
}

TEST_CASE("BasicWordSplit splits on Unicode punctuation and spaces like the BERT pre-tokenizer",
          "[ner][tokenizer][utf8]") {
    // German quotes and the en dash are punctuation: the name is a word of its
    // own, as the multilingual models were trained on
    auto quoted = BasicWordSplit("\xE2\x80\x9ESabine Wei\xC3\x9F\xE2\x80\x9C \xE2\x80\x93 Allianz");
    std::vector<std::string> expected_quoted = {"\xE2\x80\x9E", "Sabine", "Wei\xC3\x9F", "\xE2\x80\x9C",
                                                "\xE2\x80\x93", "Allianz"};
    REQUIRE(quoted == expected_quoted);

    // Guillemets (U+00AB) separate words; no-break space (U+00A0) is a space
    auto guillemet = BasicWordSplit("a\xC2\xAB" "b\xC2\xA0" "c");
    std::vector<std::string> expected_guillemet = {"a", "\xC2\xAB", "b", "c"};
    REQUIRE(guillemet == expected_guillemet);

    // Inverted question mark (Spanish) and CJK ideographs are single words
    auto spanish = BasicWordSplit("\xC2\xBF" "Dónde?");
    std::vector<std::string> expected_spanish = {"\xC2\xBF", "D\xC3\xB3nde", "?"};
    REQUIRE(spanish == expected_spanish);
    auto chinese = BasicWordSplit("\xE5\x8C\x97\xE4\xBA\xAC" "AB");
    std::vector<std::string> expected_chinese = {"\xE5\x8C\x97", "\xE4\xBA\xAC", "AB"};
    REQUIRE(chinese == expected_chinese);

    // The euro sign is a symbol, not punctuation, in BERT as here
    auto euro = BasicWordSplit("100\xE2\x82\xAC");
    REQUIRE(euro.size() == 1);
}
