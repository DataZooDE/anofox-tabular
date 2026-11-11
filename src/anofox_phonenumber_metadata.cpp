#include "anofox_phonenumber_metadata.hpp"

#include "anofox_trace.hpp"
#include "duckdb/common/assert.hpp"
#include "re2/re2.h"

#include <algorithm>
#include <mutex>

namespace duckdb {
namespace anofox {
namespace phonenumber {

#include "anofox_phonenumber_metadata_data.inc"

CompiledRegionPatterns::CompiledRegionPatterns() = default;
CompiledRegionPatterns::~CompiledRegionPatterns() = default;
CompiledRegionPatterns::CompiledRegionPatterns(CompiledRegionPatterns &&) noexcept = default;

namespace {

using duckdb_re2::RE2;
using duckdb_re2::StringPiece;

std::unique_ptr<RE2> CompilePattern(std::string_view pattern, const CountryMetadata &metadata) {
	if (pattern.empty()) {
		return nullptr;
	}
	RE2::Options options;
	options.set_log_errors(false);
	auto regex = std::make_unique<RE2>(StringPiece(pattern.data(), pattern.size()), options);
	if (!regex->ok()) {
		// Invalid metadata pattern: treat as absent rather than failing per row
		AnofoxTrace(AnofoxLogLevel::Warn, "PhoneNumber metadata: pattern of region " +
		                                      std::string(metadata.region_code) + " does not compile: " +
		                                      regex->error());
		return nullptr;
	}
	return regex;
}

std::vector<CompiledNumberFormat> CompileFormats(const NumberFormat *formats, size_t count,
                                                 const CountryMetadata &metadata) {
	std::vector<CompiledNumberFormat> compiled;
	compiled.reserve(count);
	for (size_t i = 0; i < count; i++) {
		auto pattern = CompilePattern(formats[i].pattern, metadata);
		if (!pattern) {
			continue;
		}
		compiled.push_back({CompilePattern(formats[i].leading_digits, metadata), std::move(pattern), &formats[i]});
	}
	return compiled;
}

CompiledRegionPatterns CompileRegion(const CountryMetadata &metadata) {
	CompiledRegionPatterns patterns;
	patterns.international_prefix = CompilePattern(metadata.international_prefix, metadata);
	patterns.national_prefix_for_parsing = CompilePattern(metadata.national_prefix_for_parsing, metadata);
	patterns.leading_digits = CompilePattern(metadata.leading_digits, metadata);
	for (size_t i = 0; i < NUMBER_DESC_COUNT; i++) {
		patterns.descs[i] = CompilePattern(metadata.descs[i].pattern, metadata);
	}
	patterns.national_formats = CompileFormats(metadata.national_formats, metadata.national_format_count, metadata);
	patterns.intl_formats = CompileFormats(metadata.intl_formats, metadata.intl_format_count, metadata);
	AnofoxTrace(AnofoxLogLevel::Debug, "PhoneNumber metadata: compiled patterns of region " +
	                                       std::string(metadata.region_code));
	return patterns;
}

//! One lazily compiled slot per region; std::call_once makes the first use
//! thread-safe and later reads lock-free.
struct CompiledSlot {
	std::once_flag once;
	std::unique_ptr<CompiledRegionPatterns> patterns;
};

CompiledSlot *CompiledSlots() {
	static CompiledSlot slots[REGION_COUNT + NONGEO_COUNT];
	return slots;
}

bool AnchoredMatch(const RE2 &regex, const std::string &text, RE2::Anchor anchor, StringPiece *groups = nullptr,
                   int group_count = 0) {
	return regex.Match(StringPiece(text.data(), text.size()), 0, text.size(), anchor, groups, group_count);
}

} // namespace

std::string ExpandGroups(std::string_view rule, const StringPiece *groups, int group_count) {
	std::string expanded;
	for (size_t i = 0; i < rule.size(); i++) {
		if (rule[i] == '$' && i + 1 < rule.size() && rule[i + 1] >= '0' && rule[i + 1] <= '9') {
			const int group = rule[i + 1] - '0';
			if (group < group_count && groups[group].data()) {
				expanded.append(groups[group].data(), groups[group].size());
			}
			i++;
		} else {
			expanded += rule[i];
		}
	}
	return expanded;
}

const char *MetadataVersion() {
	return PHONE_METADATA_VERSION;
}

const CountryMetadata *GetMetadataForRegion(std::string_view region_code) {
	const auto *begin = REGION_METADATA_TABLE;
	const auto *end = REGION_METADATA_TABLE + REGION_COUNT;
	const auto *it = std::lower_bound(begin, end, region_code, [](const CountryMetadata &metadata,
	                                                              std::string_view code) {
		return metadata.region_code < code;
	});
	if (it != end && it->region_code == region_code) {
		return it;
	}
	return nullptr;
}

const CountryMetadata *GetMetadataForCountryCodeRegion(int country_code, std::string_view region_code) {
	if (region_code != NON_GEO_REGION) {
		return GetMetadataForRegion(region_code);
	}
	const auto *begin = REGION_METADATA_TABLE + REGION_COUNT;
	const auto *end = begin + NONGEO_COUNT;
	const auto *it = std::lower_bound(begin, end, country_code, [](const CountryMetadata &metadata, int code) {
		return metadata.country_code < code;
	});
	if (it != end && it->country_code == country_code) {
		return it;
	}
	return nullptr;
}

const CountryCodeRegions *GetRegionsForCountryCode(int country_code) {
	const auto *begin = COUNTRY_CODE_TABLE;
	const auto *end = COUNTRY_CODE_TABLE + COUNTRY_CODE_COUNT;
	const auto *it = std::lower_bound(begin, end, country_code, [](const CountryCodeRegions &entry, int code) {
		return entry.country_code < code;
	});
	if (it != end && it->country_code == country_code && it->region_count > 0) {
		return it;
	}
	return nullptr;
}

std::string GetMainRegionForCountryCode(int country_code) {
	const auto *entry = GetRegionsForCountryCode(country_code);
	return entry ? std::string(entry->regions[0]) : std::string();
}

const CompiledRegionPatterns &GetCompiledPatterns(const CountryMetadata &metadata) {
	const auto index = static_cast<size_t>(&metadata - REGION_METADATA_TABLE);
	D_ASSERT(index < REGION_COUNT + NONGEO_COUNT);
	auto &slot = CompiledSlots()[index];
	std::call_once(slot.once,
	               [&]() { slot.patterns = std::make_unique<CompiledRegionPatterns>(CompileRegion(metadata)); });
	return *slot.patterns;
}

bool IsNumberMatchingDesc(const std::string &national_number, const CountryMetadata &metadata, NumberDescIndex desc) {
	const auto &number_desc = metadata.descs[desc];
	if (number_desc.possible_lengths != 0 && !number_desc.HasLength(national_number.size())) {
		return false;
	}
	const auto &regex = GetCompiledPatterns(metadata).descs[desc];
	return regex && AnchoredMatch(*regex, national_number, RE2::ANCHOR_BOTH);
}

size_t InternationalPrefixLength(const std::string &digits, const CountryMetadata &metadata) {
	const auto &regex = GetCompiledPatterns(metadata).international_prefix;
	StringPiece match;
	if (!regex || !AnchoredMatch(*regex, digits, RE2::ANCHOR_START, &match, 1)) {
		return 0;
	}
	return match.size();
}

const NumberFormat *ApplyNumberFormat(const std::vector<CompiledNumberFormat> &formats,
                                      const std::string &national_number, std::string &formatted) {
	for (const auto &candidate : formats) {
		// leading_digits only has to match a prefix of the national number
		if (candidate.leading_digits && !AnchoredMatch(*candidate.leading_digits, national_number, RE2::ANCHOR_START)) {
			continue;
		}
		const int group_count = std::min(candidate.pattern->NumberOfCapturingGroups() + 1, MAX_PATTERN_GROUPS);
		StringPiece groups[MAX_PATTERN_GROUPS];
		if (!AnchoredMatch(*candidate.pattern, national_number, RE2::ANCHOR_BOTH, groups, group_count)) {
			continue;
		}
		// The pattern matches the whole NSN, so this is Java's Matcher.replaceAll(format)
		formatted = ExpandGroups(candidate.format->format, groups, group_count);
		return candidate.format;
	}
	return nullptr;
}

} // namespace phonenumber
} // namespace anofox
} // namespace duckdb
