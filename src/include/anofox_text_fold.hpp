#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace duckdb {
namespace anofox {

//! Maps the full-width forms U+FF01..U+FF5E to their ASCII counterparts and the
//! ideographic space U+3000 to a blank, as Unicode NFKC does: identifiers typed
//! with an East Asian input method ("０３０ １２３４５６７８", "ＤＥ８１１…")
//! read like ASCII. Other non-ASCII characters are left unchanged. Shared by the
//! phone number and VAT parsers.
inline std::string FoldFullWidthForms(std::string_view input) {
	if (std::all_of(input.begin(), input.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
		return std::string(input);
	}
	std::string folded;
	folded.reserve(input.size());
	for (size_t i = 0; i < input.size(); i++) {
		const auto b0 = static_cast<unsigned char>(input[i]);
		if (i + 2 < input.size() && (b0 == 0xEF || b0 == 0xE3)) {
			const auto b1 = static_cast<unsigned char>(input[i + 1]);
			const auto b2 = static_cast<unsigned char>(input[i + 2]);
			// U+3000 IDEOGRAPHIC SPACE = E3 80 80
			if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80) {
				folded += ' ';
				i += 2;
				continue;
			}
			// U+FF01..U+FF3F = EF BC 81..BF, U+FF40..U+FF5E = EF BD 80..9E
			if (b0 == 0xEF && ((b1 == 0xBC && b2 >= 0x81 && b2 <= 0xBF) || (b1 == 0xBD && b2 >= 0x80 && b2 <= 0x9E))) {
				const unsigned codepoint = 0xFF00u + ((b1 - 0xBCu) << 6) + (b2 - 0x80u);
				folded += static_cast<char>(codepoint - 0xFF00u + 0x20u);
				i += 2;
				continue;
			}
		}
		folded += input[i];
	}
	return folded;
}

} // namespace anofox
} // namespace duckdb
