#pragma once

#include <algorithm>
#include <string>
#include <string_view>

namespace duckdb {
namespace anofox {

//! Length of the no-break space at `pos` (UTF-8): U+00A0 NO-BREAK SPACE (2 bytes),
//! U+2007 FIGURE SPACE or U+202F NARROW NO-BREAK SPACE (3 bytes); 0 when there is none.
//! Values copied from Excel, Word or a web page carry them in place of a plain space.
inline size_t NoBreakSpaceLength(std::string_view input, size_t pos) {
	const auto byte_at = [&](size_t i) { return static_cast<unsigned char>(input[i]); };
	if (pos + 1 < input.size() && byte_at(pos) == 0xC2 && byte_at(pos + 1) == 0xA0) {
		return 2;
	}
	if (pos + 2 < input.size() && byte_at(pos) == 0xE2 && byte_at(pos + 1) == 0x80 &&
	    (byte_at(pos + 2) == 0x87 || byte_at(pos + 2) == 0xAF)) {
		return 3;
	}
	return 0;
}

//! Removes ASCII whitespace (space, tab, line breaks, \v, \f) and the no-break spaces
//! U+00A0, U+2007 and U+202F from both ends of `input`.
inline std::string_view TrimUnicodeSpaces(std::string_view input) {
	constexpr std::string_view ASCII_WHITESPACE = " \t\r\n\v\f";
	bool trimmed = true;
	while (trimmed && !input.empty()) {
		trimmed = false;
		if (ASCII_WHITESPACE.find(input.front()) != std::string_view::npos) {
			input.remove_prefix(1);
			trimmed = true;
		} else if (const auto len = NoBreakSpaceLength(input, 0)) {
			input.remove_prefix(len);
			trimmed = true;
		}
	}
	trimmed = true;
	while (trimmed && !input.empty()) {
		trimmed = false;
		if (ASCII_WHITESPACE.find(input.back()) != std::string_view::npos) {
			input.remove_suffix(1);
			trimmed = true;
		} else if (input.size() >= 2 && NoBreakSpaceLength(input, input.size() - 2) == 2) {
			input.remove_suffix(2);
			trimmed = true;
		} else if (input.size() >= 3 && NoBreakSpaceLength(input, input.size() - 3) == 3) {
			input.remove_suffix(3);
			trimmed = true;
		}
	}
	return input;
}

//! Maps the full-width forms U+FF01..U+FF5E to their ASCII counterparts and the
//! ideographic space U+3000 and the no-break spaces U+00A0, U+2007 and U+202F to a
//! blank, as Unicode NFKC does: identifiers typed with an East Asian input method
//! ("０３０ １２３４５６７８", "ＤＥ８１１…") or copied from a spreadsheet
//! ("+49<NBSP>30<NBSP>12345678") read like ASCII. Other non-ASCII characters are left
//! unchanged. Shared by the phone number and VAT parsers.
inline std::string FoldFullWidthForms(std::string_view input) {
	if (std::all_of(input.begin(), input.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
		return std::string(input);
	}
	std::string folded;
	folded.reserve(input.size());
	for (size_t i = 0; i < input.size(); i++) {
		if (const auto nbsp_length = NoBreakSpaceLength(input, i)) {
			folded += ' ';
			i += nbsp_length - 1;
			continue;
		}
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
