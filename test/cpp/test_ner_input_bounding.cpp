//===----------------------------------------------------------------------===//
// Catch2 unit tests for NER input bounding (issue #56).
//
// Covers the header-only helpers used by NERModelManager::RunSingleInference:
//  - TruncateTokenizedInput: bounds token sequences at the model's maximum
//    sequence length while keeping the trailing special token well-formed
//  - ValidateLogitsShape: rejects output tensors whose shape does not match
//    seq_length x NUM_LABELS before any logits are read
//  - BasicWordSplit: documents the current ASCII-focused word splitting of
//    the hand-written WordPiece tokenizer (UTF-8 bytes are kept intact inside
//    words, but non-ASCII punctuation/whitespace is NOT recognized)
//
// Built into anofox_tabular_cpp_tests; no OpenVINO runtime required.
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "anofox_ner.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using duckdb::anofox::BasicWordSplit;
using duckdb::anofox::NER_MAX_SEQ_LENGTH;
using duckdb::anofox::TokenizedInput;
using duckdb::anofox::TruncateTokenizedInput;
using duckdb::anofox::ValidateLogitsShape;

namespace {

constexpr int64_t SEP_ID = 102;

/// Build a tokenized input of `count` tokens with contiguous one-byte offsets,
/// mimicking [CLS] tok ... tok [SEP] produced by the WordPiece tokenizer.
TokenizedInput MakeSequence(size_t count) {
    TokenizedInput input;
    for (size_t i = 0; i < count; ++i) {
        input.ids.push_back(static_cast<int64_t>(1000 + i));
        input.offsets.push_back({i, i + 1});
    }
    if (!input.ids.empty()) {
        input.ids.back() = SEP_ID;
        input.offsets.back() = {count, count};
    }
    return input;
}

} // namespace

TEST_CASE("TruncateTokenizedInput leaves short sequences untouched", "[ner][bounding]") {
    auto input = MakeSequence(10);
    auto expected_ids = input.ids;
    auto expected_offsets = input.offsets;

    TruncateTokenizedInput(input, NER_MAX_SEQ_LENGTH, SEP_ID);

    REQUIRE(input.ids == expected_ids);
    REQUIRE(input.offsets == expected_offsets);
}

TEST_CASE("TruncateTokenizedInput leaves exactly-max sequences untouched", "[ner][bounding]") {
    auto input = MakeSequence(NER_MAX_SEQ_LENGTH);
    auto expected_ids = input.ids;

    TruncateTokenizedInput(input, NER_MAX_SEQ_LENGTH, SEP_ID);

    REQUIRE(input.ids == expected_ids);
    REQUIRE(input.offsets.size() == input.ids.size());
}

TEST_CASE("TruncateTokenizedInput bounds long sequences at max length", "[ner][bounding]") {
    auto input = MakeSequence(NER_MAX_SEQ_LENGTH + 300);
    auto original = input;

    TruncateTokenizedInput(input, NER_MAX_SEQ_LENGTH, SEP_ID);

    // Bounded to the model limit, ids and offsets stay aligned
    REQUIRE(input.ids.size() == NER_MAX_SEQ_LENGTH);
    REQUIRE(input.offsets.size() == NER_MAX_SEQ_LENGTH);

    // The kept prefix is unchanged (truncation drops whole tokens only)
    for (size_t i = 0; i + 1 < NER_MAX_SEQ_LENGTH; ++i) {
        REQUIRE(input.ids[i] == original.ids[i]);
        REQUIRE(input.offsets[i] == original.offsets[i]);
    }

    // The sequence still ends with the special end-of-sequence token, carrying
    // an empty offset range (special tokens have no text position)
    REQUIRE(input.ids.back() == SEP_ID);
    REQUIRE(input.offsets.back().first == input.offsets.back().second);
}

TEST_CASE("TruncateTokenizedInput with max length 0 is a no-op", "[ner][bounding]") {
    auto input = MakeSequence(8);
    auto expected_ids = input.ids;

    TruncateTokenizedInput(input, 0, SEP_ID);

    REQUIRE(input.ids == expected_ids);
}

TEST_CASE("TruncateTokenizedInput is UTF-8 safe with multibyte offsets", "[ner][bounding][utf8]") {
    // Offsets as produced for multibyte text: each token spans a multibyte
    // character sequence; truncation must never invent offsets that cut into
    // these spans — every retained offset pair must already exist in the input.
    TokenizedInput input;
    input.ids.push_back(101); // [CLS]
    input.offsets.push_back({0, 0});
    size_t pos = 0;
    const size_t token_bytes = 7; // e.g. "Müller " = 7 bytes (ü is 2 bytes)
    const size_t max_len = 16;
    for (size_t i = 0; i < 40; ++i) {
        input.ids.push_back(static_cast<int64_t>(2000 + i));
        input.offsets.push_back({pos, pos + token_bytes});
        pos += token_bytes;
    }
    input.ids.push_back(SEP_ID);
    input.offsets.push_back({pos, pos});

    auto original_offsets = input.offsets;
    TruncateTokenizedInput(input, max_len, SEP_ID);

    REQUIRE(input.ids.size() == max_len);
    REQUIRE(input.offsets.size() == max_len);
    // All content offsets are unchanged token-boundary pairs from the input
    for (size_t i = 0; i + 1 < max_len; ++i) {
        REQUIRE(input.offsets[i] == original_offsets[i]);
    }
    // Final token is the end-of-sequence marker with an empty range
    REQUIRE(input.ids.back() == SEP_ID);
    REQUIRE(input.offsets.back().first == input.offsets.back().second);
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

TEST_CASE("BasicWordSplit keeps UTF-8 multibyte sequences intact (documented limitation)",
          "[ner][tokenizer][utf8]") {
    // The splitter classifies single bytes with std::isspace/std::ispunct, so
    // it only understands ASCII separators. UTF-8 continuation bytes are
    // never split apart — multibyte words survive as complete byte sequences.
    auto words = BasicWordSplit("Herr Müller wohnt in Köln");
    std::vector<std::string> expected = {"Herr", "M\xC3\xBCller", "wohnt", "in", "K\xC3\xB6ln"};
    REQUIRE(words == expected);

    // Documented limitation: non-ASCII punctuation (here U+00AB) is NOT a
    // word boundary — the surrounding text stays one token.
    // Split the literal so the \xAB hex escape does not greedily absorb the
    // following 'b' (also a hex digit) into an out-of-range \xABb escape, which
    // clang rejects as a hard error.
    auto guillemet = BasicWordSplit("a\xC2\xAB" "b");
    REQUIRE(guillemet.size() == 1);
    REQUIRE(guillemet[0] == "a\xC2\xAB" "b");
}
