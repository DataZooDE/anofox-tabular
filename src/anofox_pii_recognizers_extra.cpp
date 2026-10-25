// PII recognizers for German insurance data (finding F24): DATE_OF_BIRTH,
// DE_SVNR, DE_ID_CARD, POSTAL_CODE, plus the checksum-failure search behind
// anofox_pii_require_checksum = false for IBAN and CREDIT_CARD.
//
// All of these are context-gated or checksum-validated: a plain date, a
// 9-character token or a 5-digit number is not PII on its own, and claims
// notes are full of invoice dates, order numbers and amounts.

#include "anofox_pii.hpp"
#include "anofox_trace.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

// Confidence of a value that has the shape of an IBAN or card number but fails
// its checksum (reported only with anofox_pii_require_checksum = false)
constexpr double CHECKSUM_FAILURE_CONFIDENCE = 0.6;

// How far before a value a context keyword may start, in bytes
constexpr size_t KEYWORD_WINDOW = 80;

bool IsAsciiDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

// A byte of a word: ASCII letter or digit, or any byte of a multi-byte UTF-8
// character (umlauts)
bool IsWordChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || u >= 0x80;
}

bool IsLetterChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalpha(u) || u >= 0x80;
}

std::string UpperAscii(std::string_view text) {
    std::string out(text);
    for (auto &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (auto &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string WithoutSpaces(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            out += c;
        }
    }
    return out;
}

// Replace every letter and digit with '*' (one per code point), keeping
// separators and spaces; the first `keep` letters/digits and the byte range
// [keep_from, keep_to) stay in clear text.
std::string MaskAlnum(const std::string &text, size_t keep, size_t keep_from = std::string::npos,
                      size_t keep_to = std::string::npos) {
    std::string out;
    out.reserve(text.size());
    size_t kept = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        const auto u = static_cast<unsigned char>(c);
        if (keep_from != std::string::npos && i >= keep_from && i < keep_to) {
            out += c;
            continue;
        }
        if (!IsWordChar(c)) {
            out += c;
            continue;
        }
        if (u >= 0x80 && (u & 0xC0) == 0x80) {
            continue;  // continuation byte of a character already handled
        }
        if (kept < keep) {
            // copy the whole code point
            out += c;
            while (i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
                out += text[++i];
            }
            kept++;
            continue;
        }
        out += '*';
    }
    return out;
}

/**
 * True when a keyword match ends shortly before `pos` with only filler words
 * between keyword and value: at most `max_words` words, at most `max_gap`
 * bytes, no digit (another value in between) and none of `forbidden` (a
 * comma or sentence end separates keyword and value). Keywords must stand as
 * whole words: "Gebühr" does not contain the keyword "geb".
 */
bool KeywordPrecedes(const std::string &text, size_t pos, const std::regex &keyword, size_t max_words,
                     size_t max_gap, std::string_view forbidden) {
    const size_t window_start = pos > KEYWORD_WINDOW ? pos - KEYWORD_WINDOW : 0;
    auto begin = text.begin() + static_cast<std::ptrdiff_t>(window_start);
    auto end = text.begin() + static_cast<std::ptrdiff_t>(pos);

    size_t keyword_end = std::string::npos;
    for (std::sregex_iterator it(begin, end, keyword), stop; it != stop; ++it) {
        const size_t start = window_start + static_cast<size_t>(it->position());
        const size_t finish = start + static_cast<size_t>(it->length());
        if (finish == start) {
            continue;
        }
        if (start > 0 && IsWordChar(text[start - 1])) {
            continue;  // keyword inside a longer word ("ungeboren")
        }
        if (IsWordChar(text[finish - 1]) && finish < text.size() && IsWordChar(text[finish])) {
            continue;  // longer word ("Gebühr", "Geburtstagsfeier")
        }
        keyword_end = finish;  // keep the last one
    }
    if (keyword_end == std::string::npos || pos - keyword_end > max_gap) {
        return false;
    }

    size_t words = 0;
    bool in_word = false;
    for (size_t i = keyword_end; i < pos; i++) {
        const char c = text[i];
        if (IsAsciiDigit(c) || forbidden.find(c) != std::string_view::npos) {
            return false;
        }
        const bool letter = IsLetterChar(c);
        if (letter && !in_word) {
            words++;
        }
        in_word = letter;
    }
    return words <= max_words;
}

std::regex CaseSensitive(const char *pattern) {
    return std::regex(pattern, std::regex_constants::ECMAScript);
}

std::regex CaseInsensitive(const char *pattern) {
    return std::regex(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
}

// ----------------------------------------------------------------------------
// Dates
// ----------------------------------------------------------------------------

// Month names (German and English, with abbreviations), shared by the date
// pattern and the month lookup
#define ANOFOX_PII_MONTHS                                                                                          \
	"(?:Januar|January|Jan|Februar|February|Feb|M\xC3\xA4rz|Maerz|March|M\xC3\xA4r|Mar|April|Apr|Mai|May|Juni|June|" \
	"Jun|Juli|July|Jul|August|Aug|September|Sept|Sep|Oktober|October|Okt|Oct|November|Nov|Dezember|December|Dez|"   \
	"Dec)"

const char *DATE_PATTERN =
    // 12.03.1985, 3.7.1962, 12. 03. 1985
    R"(\b(?:\d{1,2}\. ?\d{1,2}\. ?\d{4})"
    // 1985-03-12
    R"(|\d{4}-\d{2}-\d{2})"
    // 12/03/1985
    R"(|\d{1,2}/\d{1,2}/\d{4})"
    // 12. März 1985, 12 March 1985
    R"(|\d{1,2}\.? ?)" ANOFOX_PII_MONTHS R"(\.? \d{4})"
    // March 12, 1985
    R"(|)" ANOFOX_PII_MONTHS R"(\.? \d{1,2}(?:st|nd|rd|th)?,? \d{4})"
    R"()\b)";

const char *BIRTH_KEYWORD_PATTERN =
    R"(geburtsdatum|geburtstag|geboren|geb\.?-?datum|geb\.?|date\s+of\s+birth|birth\s*date|birthday|born|d\.o\.b\.?|dob)";

int MonthFromName(const std::string &word) {
    const std::string lower = LowerAscii(word);
    static const std::array<std::pair<const char *, int>, 15> prefixes = {{{"jan", 1},
                                                                            {"feb", 2},
                                                                            {"m\xC3\xA4r", 3},
                                                                            {"mae", 3},
                                                                            {"mar", 3},
                                                                            {"apr", 4},
                                                                            {"mai", 5},
                                                                            {"may", 5},
                                                                            {"jun", 6},
                                                                            {"jul", 7},
                                                                            {"aug", 8},
                                                                            {"sep", 9},
                                                                            {"okt", 10},
                                                                            {"oct", 10},
                                                                            {"nov", 11}}};
    for (const auto &entry : prefixes) {
        if (lower.compare(0, std::char_traits<char>::length(entry.first), entry.first) == 0) {
            return entry.second;
        }
    }
    if (lower.compare(0, 3, "dez") == 0 || lower.compare(0, 3, "dec") == 0) {
        return 12;
    }
    return 0;
}

// A real calendar date between 1900-01-01 and today
bool IsPlausibleBirthDate(int year, int month, int day) {
    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    if (!Date::IsValid(year, month, day)) {
        return false;
    }
    int32_t today_year = 0, today_month = 0, today_day = 0;
    Date::Convert(Timestamp::GetDate(Timestamp::GetCurrentTimestamp()), today_year, today_month, today_day);
    if (year != today_year) {
        return year < today_year;
    }
    if (month != today_month) {
        return month < today_month;
    }
    return day <= today_day;
}

// ----------------------------------------------------------------------------
// IBAN country lengths (SWIFT IBAN registry)
// ----------------------------------------------------------------------------

size_t IbanLengthForCountry(const std::string &country) {
    static const std::unordered_map<std::string, size_t> lengths = {
        {"AD", 24}, {"AE", 23}, {"AL", 28}, {"AT", 20}, {"AZ", 28}, {"BA", 20}, {"BE", 16}, {"BG", 22},
        {"BH", 22}, {"BI", 27}, {"BR", 29}, {"BY", 28}, {"CH", 21}, {"CR", 22}, {"CY", 28}, {"CZ", 24},
        {"DE", 22}, {"DJ", 27}, {"DK", 18}, {"DO", 28}, {"EE", 20}, {"EG", 29}, {"ES", 24}, {"FI", 18},
        {"FK", 18}, {"FO", 18}, {"FR", 27}, {"GB", 22}, {"GE", 22}, {"GI", 23}, {"GL", 18}, {"GR", 27},
        {"GT", 28}, {"HR", 21}, {"HU", 28}, {"IE", 22}, {"IL", 23}, {"IQ", 23}, {"IS", 26}, {"IT", 27},
        {"JO", 30}, {"KW", 30}, {"KZ", 20}, {"LB", 28}, {"LC", 32}, {"LI", 21}, {"LT", 20}, {"LU", 20},
        {"LV", 21}, {"LY", 25}, {"MC", 27}, {"MD", 24}, {"ME", 22}, {"MK", 19}, {"MN", 20}, {"MR", 27},
        {"MT", 31}, {"MU", 30}, {"NI", 28}, {"NL", 18}, {"NO", 15}, {"OM", 23}, {"PK", 24}, {"PL", 28},
        {"PS", 29}, {"PT", 25}, {"QA", 29}, {"RO", 24}, {"RS", 22}, {"RU", 33}, {"SA", 24}, {"SC", 31},
        {"SD", 18}, {"SE", 24}, {"SI", 19}, {"SK", 24}, {"SM", 27}, {"SO", 23}, {"ST", 25}, {"SV", 28},
        {"TL", 23}, {"TN", 24}, {"TR", 26}, {"UA", 29}, {"VA", 22}, {"VG", 24}, {"XK", 20}, {"YE", 30}};
    auto it = lengths.find(country);
    return it == lengths.end() ? 0 : it->second;
}

size_t CompactLength(const std::string &text) {
    return static_cast<size_t>(std::count_if(text.begin(), text.end(), [](char c) { return c != ' ' && c != '-'; }));
}

// ----------------------------------------------------------------------------
// Postal codes
// ----------------------------------------------------------------------------

// Capitalised words that follow numbers without being a town
bool IsQuantityWord(const std::string &word) {
    static const std::unordered_set<std::string> words = {
        "st\xC3\xBC" "ck", "stueck", "stk", "euro", "franken", "dollar", "pfund", "prozent", "jahre", "jahren",
        "monate", "monaten", "wochen", "tage", "tagen", "stunden", "minuten", "sekunden", "kilometer", "meter",
        "liter", "kilo", "gramm", "tonnen", "kunden", "mitarbeiter", "personen", "einwohner", "versicherte",
        "f\xC3\xA4lle", "faelle", "sch\xC3\xA4" "den", "schaeden", "punkte", "teile", "artikel", "einheiten",
        "vertr\xC3\xA4ge", "vertraege", "mal", "seiten", "zeilen", "zeichen", "dokumente", "anfragen",
        "rechnungen", "positionen", "units", "pieces", "items", "customers", "people", "years", "days"};
    return words.count(LowerAscii(word)) > 0;
}

// A town name after a postal code: a capitalised word ("München", "Wien",
// "Bad Homburg") that is not a quantity word ("Stück", "Euro")
bool FollowedByTown(const std::string &text, size_t pos) {
    size_t i = pos;
    if (i >= text.size() || text[i] != ' ') {
        return false;
    }
    while (i < text.size() && text[i] == ' ') {
        i++;
    }
    if (i >= text.size()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(text[i]);
    const bool upper_ascii = std::isupper(first) != 0;
    // Ä, Ö, Ü in UTF-8: C3 84, C3 96, C3 9C
    const bool upper_umlaut = first == 0xC3 && i + 1 < text.size() &&
                              (static_cast<unsigned char>(text[i + 1]) == 0x84 ||
                               static_cast<unsigned char>(text[i + 1]) == 0x96 ||
                               static_cast<unsigned char>(text[i + 1]) == 0x9C);
    if (!upper_ascii && !upper_umlaut) {
        return false;
    }
    size_t end = i;
    while (end < text.size() && (IsLetterChar(text[end]) || text[end] == '-')) {
        end++;
    }
    const std::string word = text.substr(i, end - i);
    bool has_lower = false;
    for (char c : word) {
        has_lower = has_lower || std::islower(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80;
    }
    return word.size() >= 3 && has_lower && !IsQuantityWord(word);
}

// A town name right before the code ("London SW1A 2AA")
bool PrecededByTown(const std::string &text, size_t pos) {
    size_t end = pos;
    if (end == 0 || text[end - 1] != ' ') {
        return false;
    }
    while (end > 0 && text[end - 1] == ' ') {
        end--;
    }
    size_t start = end;
    while (start > 0 && (IsLetterChar(text[start - 1]) || text[start - 1] == '-')) {
        start--;
    }
    if (start == end || !std::isupper(static_cast<unsigned char>(text[start]))) {
        return false;
    }
    const std::string word = text.substr(start, end - start);
    const bool has_lower = std::any_of(word.begin(), word.end(), [](char c) {
        return std::islower(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80;
    });
    return word.size() >= 3 && has_lower && !IsQuantityWord(word);
}

// The value starts the text or a line, or follows ", " (an address line)
bool StartsAddressPart(const std::string &text, size_t pos) {
    size_t i = pos;
    while (i > 0 && text[i - 1] == ' ') {
        i--;
    }
    return i == 0 || text[i - 1] == '\n' || text[i - 1] == '\r' || text[i - 1] == ',' || text[i - 1] == ';';
}

bool EndsAddressPart(const std::string &text, size_t pos) {
    return pos >= text.size() || text[pos] == '\n' || text[pos] == '\r' || text[pos] == ',' || text[pos] == ';' ||
           text[pos] == ')' || (text[pos] == '.' && (pos + 1 >= text.size() || text[pos + 1] == ' '));
}

} // namespace

// ============================================================================
// Checksum failures for IBAN and CREDIT_CARD (anofox_pii_require_checksum)
// ============================================================================

std::vector<PIIMatch> CreditCardRecognizer::FindChecksumFailures(const std::string &text) const {
    // Same candidates as FindMatches (digit chains with a brand prefix and a
    // brand length, standing on their own), only those whose Luhn check fails
    return ScanCardChains(text, true, CHECKSUM_FAILURE_CONFIDENCE);
}

std::vector<PIIMatch> IBANRecognizer::FindChecksumFailures(const std::string &text) const {
    std::vector<PIIMatch> matches;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern_), stop; it != stop; ++it) {
        std::string candidate = it->str();
        const std::string separator = (*it)[1].str();
        const auto start = static_cast<size_t>(it->position());

        // Skip what FindMatches reports (it trims trailing groups until the
        // checksum holds)
        bool valid = Mod97Check(candidate);
        for (std::string trimmed = candidate; !valid && !separator.empty();) {
            auto cut = trimmed.rfind(separator[0]);
            if (cut == std::string::npos || cut < 4) {
                break;
            }
            trimmed.resize(cut);
            valid = Mod97Check(trimmed);
        }
        if (valid) {
            continue;
        }

        // Without a checksum, the length registered for the country decides
        // where the IBAN ends; unknown countries are not reported
        const size_t expected = IbanLengthForCountry(UpperAscii(candidate.substr(0, 2)));
        if (expected == 0) {
            continue;
        }
        while (CompactLength(candidate) > expected && !separator.empty()) {
            auto cut = candidate.rfind(separator[0]);
            if (cut == std::string::npos || cut < 4) {
                break;
            }
            candidate.resize(cut);
        }
        if (CompactLength(candidate) != expected) {
            continue;
        }
        AnofoxTrace(AnofoxLogLevel::Trace, "pii: IBAN candidate fails MOD-97, kept (require_checksum=false)");
        matches.emplace_back(type_, candidate, start, start + candidate.size(), CHECKSUM_FAILURE_CONFIDENCE);
    }
    return matches;
}

// ============================================================================
// DATE_OF_BIRTH
// ============================================================================

DateOfBirthRecognizer::DateOfBirthRecognizer()
    : RegexRecognizer(PIIType::DATE_OF_BIRTH, "Date of Birth", DATE_PATTERN),
      keyword_(CaseInsensitive(BIRTH_KEYWORD_PATTERN)) {
}

DateOfBirthRecognizer::~DateOfBirthRecognizer() {
}

std::vector<PIIMatch> DateOfBirthRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern_), stop; it != stop; ++it) {
        const auto start = static_cast<size_t>(it->position());
        const std::string candidate = it->str();
        // Up to three filler words ("geboren am", "Geburtsdatum der versicherten
        // Person:"), no comma or sentence end in between
        if (!KeywordPrecedes(text, start, keyword_, 3, 40, ".,;!?\n\r")) {
            continue;
        }
        if (!Validate(candidate)) {
            AnofoxTrace(AnofoxLogLevel::Trace, "pii: birth date candidate is no plausible date: " + candidate);
            continue;
        }
        matches.emplace_back(type_, candidate, start, start + candidate.size(), 1.0);
    }
    return matches;
}

bool DateOfBirthRecognizer::Validate(const std::string &text) const {
    std::vector<int> numbers;
    std::vector<size_t> widths;
    std::string month_word;
    for (size_t i = 0; i < text.size();) {
        if (IsAsciiDigit(text[i])) {
            size_t end = i;
            while (end < text.size() && IsAsciiDigit(text[end])) {
                end++;
            }
            if (end - i > 4) {
                return false;
            }
            numbers.push_back(std::stoi(text.substr(i, end - i)));
            widths.push_back(end - i);
            i = end;
        } else if (IsLetterChar(text[i])) {
            size_t end = i;
            while (end < text.size() && IsLetterChar(text[end])) {
                end++;
            }
            if (month_word.empty()) {
                month_word = text.substr(i, end - i);
            }
            i = end;
        } else {
            i++;
        }
    }

    if (!month_word.empty()) {
        // "12. März 1985" / "March 12, 1985": day and year around a month name
        const int month = MonthFromName(month_word);
        return month != 0 && numbers.size() == 2 && widths[1] == 4 && IsPlausibleBirthDate(numbers[1], month, numbers[0]);
    }
    if (numbers.size() != 3) {
        return false;
    }
    if (widths[0] == 4) {
        return IsPlausibleBirthDate(numbers[0], numbers[1], numbers[2]);  // ISO
    }
    if (text.find('/') != std::string::npos) {
        // DD/MM/YYYY, or the US order MM/DD/YYYY when only that one is a date
        return IsPlausibleBirthDate(numbers[2], numbers[1], numbers[0]) ||
               IsPlausibleBirthDate(numbers[2], numbers[0], numbers[1]);
    }
    return IsPlausibleBirthDate(numbers[2], numbers[1], numbers[0]);  // DD.MM.YYYY
}

std::string DateOfBirthRecognizer::GetPartialMask(const std::string &text) const {
    // Keep the year: 12.03.1985 -> **.**.1985, 1985-03-12 -> 1985-**-**
    for (size_t i = 0; i + 4 <= text.size(); i++) {
        if ((i == 0 || !IsAsciiDigit(text[i - 1])) && IsAsciiDigit(text[i]) && IsAsciiDigit(text[i + 1]) &&
            IsAsciiDigit(text[i + 2]) && IsAsciiDigit(text[i + 3]) && (i + 4 == text.size() || !IsAsciiDigit(text[i + 4]))) {
            return MaskAlnum(text, 0, i, i + 4);
        }
    }
    return MaskAlnum(text, 0);
}

// ============================================================================
// DE_SVNR (Rentenversicherungsnummer)
// ============================================================================

DESVNRRecognizer::DESVNRRecognizer()
    : RegexRecognizer(PIIType::DE_SVNR, "German Social Insurance Number",
                      // area (2 digits), birth date DDMMYY, initial of the birth
                      // name, serial (2 digits), check digit; spaces optional:
                      // 65 170839 J 003, 65 170839 J 00 3, 65170839J003
                      R"(\b\d{2} ?\d{6} ?[A-Z] ?\d{2} ?\d\b)") {
}

DESVNRRecognizer::~DESVNRRecognizer() {
}

bool DESVNRRecognizer::Validate(const std::string &text) const {
    const std::string compact = UpperAscii(WithoutSpaces(text));
    if (compact.size() != 12 || !std::isupper(static_cast<unsigned char>(compact[8]))) {
        return false;
    }
    for (size_t i = 0; i < 12; i++) {
        if (i != 8 && !IsAsciiDigit(compact[i])) {
            return false;
        }
    }
    if (compact.compare(0, 2, "00") == 0) {
        return false;  // no area 00
    }
    const int day = std::stoi(compact.substr(2, 2));
    const int month = std::stoi(compact.substr(4, 2));
    if (day < 1 || day > 31 || month < 1 || month > 12) {
        return false;
    }

    // The letter becomes its two-digit position in the alphabet (A=01 .. Z=26);
    // the 12 digits are weighted 2,1,2,5,7,1,2,1,2,1,2,1, the digit sums of
    // the products are added up, and the sum modulo 10 is the check digit.
    const int letter = compact[8] - 'A' + 1;
    std::string digits = compact.substr(0, 8);
    digits += static_cast<char>('0' + letter / 10);
    digits += static_cast<char>('0' + letter % 10);
    digits += compact.substr(9, 2);
    static const std::array<int, 12> weights = {{2, 1, 2, 5, 7, 1, 2, 1, 2, 1, 2, 1}};
    int sum = 0;
    for (size_t i = 0; i < 12; i++) {
        const int product = (digits[i] - '0') * weights[i];
        sum += product / 10 + product % 10;
    }
    return sum % 10 == compact[11] - '0';
}

std::string DESVNRRecognizer::GetPartialMask(const std::string &text) const {
    // Keep the area number: 65 170839 J 003 -> 65 ****** * ***
    return MaskAlnum(text, 2);
}

// ============================================================================
// DE_ID_CARD (Personalausweis / Reisepass)
// ============================================================================

namespace {

// ICAO 9303 check digit: values 0-9, A=10 .. Z=35, weights 7,3,1, sum mod 10
int IcaoCheckDigit(const std::string &text) {
    static const std::array<int, 3> weights = {{7, 3, 1}};
    int sum = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        const int value = IsAsciiDigit(c) ? c - '0' : c - 'A' + 10;
        sum += value * weights[i % 3];
    }
    return sum % 10;
}

const char *ID_KEYWORD_PATTERN =
    R"(personalausweis\w*|ausweis\w*|reisepass\w*|passnummer|pass-?nr\.?|identity\s+card|id[\s-]?card|passport)";

} // namespace

DEIDCardRecognizer::DEIDCardRecognizer()
    : RegexRecognizer(PIIType::DE_ID_CARD, "German ID Card / Passport", R"(\b[A-Z0-9]{9,10}\b)"),
      keyword_(CaseInsensitive(ID_KEYWORD_PATTERN)) {
    // Document numbers use the upper-case alphabet without vowels and the
    // look-alikes B, D, Q, S: a letter first, then 8 letters or digits, and an
    // optional check digit
    pattern_ = CaseSensitive(R"(\b[CFGHJKLMNPRTVWXYZ][CFGHJKLMNPRTVWXYZ0-9]{8}[0-9]?\b)");
}

DEIDCardRecognizer::~DEIDCardRecognizer() {
}

std::vector<PIIMatch> DEIDCardRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern_), stop; it != stop; ++it) {
        const auto start = static_cast<size_t>(it->position());
        const std::string candidate = it->str();
        if (!Validate(candidate)) {
            continue;
        }
        // Without its check digit the number needs a keyword before it
        // ("Ausweis-Nr.: T22000129", "Passport no. C01X00T47")
        if (candidate.size() == 9 && !KeywordPrecedes(text, start, keyword_, 3, 30, ",;!?\n\r")) {
            continue;
        }
        matches.emplace_back(type_, candidate, start, start + candidate.size(), 1.0);
    }
    return matches;
}

bool DEIDCardRecognizer::Validate(const std::string &text) const {
    static const std::string alphabet = "CFGHJKLMNPRTVWXYZ0123456789";
    if (text.size() != 9 && text.size() != 10) {
        return false;
    }
    if (!std::all_of(text.begin(), text.begin() + 9, [](char c) { return alphabet.find(c) != std::string::npos; })) {
        return false;
    }
    if (IsAsciiDigit(text[0]) || !std::any_of(text.begin(), text.begin() + 9, IsAsciiDigit)) {
        return false;
    }
    if (text.size() == 10) {
        return IsAsciiDigit(text[9]) && IcaoCheckDigit(text.substr(0, 9)) == text[9] - '0';
    }
    return true;
}

std::string DEIDCardRecognizer::GetPartialMask(const std::string &text) const {
    // Keep the first character (part of the authority code): T*********
    return MaskAlnum(text, 1);
}

// ============================================================================
// POSTAL_CODE (opt-in)
// ============================================================================

PostalCodeRecognizer::PostalCodeRecognizer()
    : RegexRecognizer(PIIType::POSTAL_CODE, "Postal Code", R"(\b\d{4,5}\b)"),
      // DE (5 digits), AT/CH (4 digits), with an optional country prefix
      dach_(CaseSensitive(R"(\b(?:(D|A|CH)-)?(\d{4,5})\b)")),
      // NL: 1234 AB (SA, SD, SS are not issued)
      nl_(CaseSensitive(R"(\b([1-9]\d{3} ?(?!SA|SD|SS)[A-Z]{2})\b)")),
      // UK: outward code, space, inward code (SW1A 2AA, M1 1AE, EC1A 1BB)
      uk_(CaseSensitive(R"(\b([A-Z]{1,2}\d[A-Z\d]? \d[ABD-HJLNP-UW-Z]{2})\b)")),
      keyword_(CaseInsensitive(R"(postleitzahl|plz|postcode|post\s+code|postal\s+code|zip(?:\s*code)?)")),
      // A street with house number right before the code on the same line:
      // "Musterstraße 12, ", "Hauptstr. 5a, ", "10 Downing Street, "
      street_(CaseInsensitive(R"((?:stra(?:\xC3\x9F|ss)e|str\.|weg|gasse|platz|allee|ring|damm|ufer|chaussee|steig|pfad|markt|gracht|straat|laan|street|road|lane|avenue)\s*\d+\s*[a-z]?\s*,?\s*$|\b\d+[a-z]?\s+[A-Za-z]+\s+(?:street|road|lane|avenue)\s*,?\s*$)")) {
}

PostalCodeRecognizer::~PostalCodeRecognizer() {
}

std::vector<PIIMatch> PostalCodeRecognizer::FindMatches(const std::string &text) const {
    std::vector<PIIMatch> matches;

    auto street_before = [&](size_t pos) {
        size_t line_start = text.rfind('\n', pos == 0 ? 0 : pos - 1);
        line_start = line_start == std::string::npos ? 0 : line_start + 1;
        if (pos - line_start > 120) {
            line_start = pos - 120;
        }
        return std::regex_search(text.begin() + static_cast<std::ptrdiff_t>(line_start),
                                 text.begin() + static_cast<std::ptrdiff_t>(pos), street_);
    };
    auto keyword_before = [&](size_t pos) { return KeywordPrecedes(text, pos, keyword_, 1, 20, ",;!?\n\r"); };

    // DE / AT / CH
    for (std::sregex_iterator it(text.begin(), text.end(), dach_), stop; it != stop; ++it) {
        const auto &m = *it;
        const auto start = static_cast<size_t>(m.position(2));
        const std::string code = m[2].str();
        const auto end = start + code.size();
        const bool five = code.size() == 5;
        if ((five && code.compare(0, 2, "00") == 0) || (!five && code[0] == '0')) {
            continue;
        }
        if (start > 0 && (text[start - 1] == '.' || text[start - 1] == '/' ||
                          (text[start - 1] == '-' && !m[1].matched))) {
            continue;  // part of a date or a reference ("15.08.2024", "AZ-2024")
        }
        bool found = false;
        if (keyword_before(static_cast<size_t>(m.position(0)))) {
            found = true;
        } else if (FollowedByTown(text, end)) {
            found = m[1].matched || street_before(static_cast<size_t>(m.position(0))) ||
                    (five && StartsAddressPart(text, start));
        }
        if (found) {
            matches.emplace_back(type_, code, start, end, 1.0);
        }
    }

    // NL
    for (std::sregex_iterator it(text.begin(), text.end(), nl_), stop; it != stop; ++it) {
        const auto start = static_cast<size_t>(it->position(1));
        const std::string code = (*it)[1].str();
        const auto end = start + code.size();
        if (keyword_before(start) ||
            (FollowedByTown(text, end) && (StartsAddressPart(text, start) || street_before(start)))) {
            matches.emplace_back(type_, code, start, end, 1.0);
        }
    }

    // UK: the code ends an address line ("..., London SW1A 2AA")
    for (std::sregex_iterator it(text.begin(), text.end(), uk_), stop; it != stop; ++it) {
        const auto start = static_cast<size_t>(it->position(1));
        const std::string code = (*it)[1].str();
        const auto end = start + code.size();
        if (keyword_before(start) || (EndsAddressPart(text, end) &&
                                      (StartsAddressPart(text, start) || PrecededByTown(text, start)))) {
            matches.emplace_back(type_, code, start, end, 1.0);
        }
    }
    return matches;
}

std::string PostalCodeRecognizer::GetPartialMask(const std::string &text) const {
    // Keep the first two characters (the region): 80331 -> 80***
    return MaskAlnum(text, 2);
}

} // namespace anofox
} // namespace duckdb
