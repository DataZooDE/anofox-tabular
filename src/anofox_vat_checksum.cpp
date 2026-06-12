// Per-country VAT check-digit validation.
//
// Only countries whose algorithm is well-documented and verified with known
// public examples are implemented here. For each implemented country the
// registry entry in anofox_vat_country.cpp sets has_checksum = true; all other
// countries honestly report has_checksum = false and are validated by syntax
// only.
//
// All functions receive the VAT payload *without* the 2-letter country prefix,
// already normalized (uppercase, alphanumeric only) and already matched
// against the country's syntax pattern.

#include "anofox_vat.hpp"

#include <cctype>
#include <cstdint>
#include <string>

namespace duckdb {

namespace {

inline bool IsDigitChar(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

inline int DigitValue(char c) {
  return c - '0';
}

// Parse a small all-digit substring into an integer. The caller guarantees
// (via the syntax regex) that the characters are digits.
int64_t ToNumber(const std::string& s, size_t pos, size_t len) {
  int64_t value = 0;
  for (size_t i = pos; i < pos + len && i < s.size(); i++) {
    if (!IsDigitChar(s[i])) {
      return -1;
    }
    value = value * 10 + DigitValue(s[i]);
  }
  return value;
}

// Austria: 'U' + 8 digits. Weights 1,2,1,2,1,2,1 over the first 7 digits,
// doubled digits are reduced by digit sum; check = (96 - total) mod 10.
bool ChecksumAT(const std::string& digits) {
  if (digits.size() != 9 || digits[0] != 'U') {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 7; i++) {
    int d = DigitValue(digits[i + 1]);
    if (i % 2 == 1) {
      d *= 2;
      d = d / 10 + d % 10;
    }
    total += d;
  }
  int check = ((96 - total) % 10 + 10) % 10;
  return DigitValue(digits[8]) == check;
}

// Belgium: 10 digits; last two = 97 - (first 8 digits mod 97).
bool ChecksumBE(const std::string& digits) {
  if (digits.size() != 10) {
    return false;
  }
  int64_t base = ToNumber(digits, 0, 8);
  int64_t check = ToNumber(digits, 8, 2);
  if (base < 0 || check < 0) {
    return false;
  }
  return check == 97 - (base % 97);
}

// Germany: 9 digits, ISO 7064 MOD 11,10. Official example: DE111111125.
bool ChecksumDE(const std::string& digits) {
  if (digits.size() != 9) {
    return false;
  }
  int product = 10;
  for (int i = 0; i < 8; i++) {
    int sum = (DigitValue(digits[i]) + product) % 10;
    if (sum == 0) {
      sum = 10;
    }
    product = (2 * sum) % 11;
  }
  int check = 11 - product;
  if (check == 10) {
    check = 0;
  }
  return DigitValue(digits[8]) == check;
}

// Denmark: 8 digits, weights 2,7,6,5,4,3,2,1; total mod 11 == 0.
bool ChecksumDK(const std::string& digits) {
  static const int kWeights[] = {2, 7, 6, 5, 4, 3, 2, 1};
  if (digits.size() != 8) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 8; i++) {
    total += kWeights[i] * DigitValue(digits[i]);
  }
  return total % 11 == 0;
}

// Spain: NIF (8 digits + letter), NIE (X/Y/Z + 7 digits + letter) and
// CIF (letter + 7 digits + check digit or letter).
bool ChecksumES(const std::string& digits) {
  static const std::string kNifLetters = "TRWAGMYFPDXBNJZSQVHLCKE";
  static const std::string kCifLetters = "JABCDEFGHI";
  if (digits.size() != 9) {
    return false;
  }
  char first = digits[0];
  char last = digits[8];
  if (IsDigitChar(first)) {
    // Personal NIF: letter = table[number mod 23].
    int64_t number = ToNumber(digits, 0, 8);
    if (number < 0) {
      return false;
    }
    return last == kNifLetters[number % 23];
  }
  if ((first == 'X' || first == 'Y' || first == 'Z') && !IsDigitChar(last)) {
    // NIE: replace X/Y/Z with 0/1/2, then NIF rule.
    int64_t number = ToNumber(digits, 1, 7);
    if (number < 0) {
      return false;
    }
    number += static_cast<int64_t>(first - 'X') * 10000000;
    return last == kNifLetters[number % 23];
  }
  // CIF: Luhn-style over the 7 middle digits.
  int total = 0;
  for (int i = 0; i < 7; i++) {
    int d = DigitValue(digits[i + 1]);
    if (i % 2 == 0) {
      d *= 2;
      d = d / 10 + d % 10;
    }
    total += d;
  }
  int check = (10 - total % 10) % 10;
  return last == static_cast<char>('0' + check) || last == kCifLetters[check];
}

// Finland: 8 digits, weights 7,9,10,5,8,4,2 over the first 7;
// check = (11 - total mod 11), remainder 1 is invalid, remainder 0 -> 0.
bool ChecksumFI(const std::string& digits) {
  static const int kWeights[] = {7, 9, 10, 5, 8, 4, 2};
  if (digits.size() != 8) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 7; i++) {
    total += kWeights[i] * DigitValue(digits[i]);
  }
  int remainder = total % 11;
  if (remainder == 1) {
    return false;
  }
  int check = (11 - remainder) % 11;
  return DigitValue(digits[7]) == check;
}

// France: 2-character key + 9-digit SIREN. For numeric keys:
// key = (12 + 3 * (SIREN mod 97)) mod 97. Alphanumeric keys are accepted
// without verification (no public algorithm with verified vectors).
bool ChecksumFR(const std::string& digits) {
  if (digits.size() != 11) {
    return false;
  }
  if (!IsDigitChar(digits[0]) || !IsDigitChar(digits[1])) {
    return true;
  }
  int64_t key = ToNumber(digits, 0, 2);
  int64_t siren = ToNumber(digits, 2, 9);
  if (key < 0 || siren < 0) {
    return false;
  }
  return key == (12 + 3 * (siren % 97)) % 97;
}

// Greece: 9 digits, weights 256,128,64,32,16,8,4,2; check = total mod 11 mod 10.
bool ChecksumGR(const std::string& digits) {
  if (digits.size() != 9) {
    return false;
  }
  int total = 0;
  int weight = 256;
  for (int i = 0; i < 8; i++) {
    total += weight * DigitValue(digits[i]);
    weight /= 2;
  }
  return DigitValue(digits[8]) == total % 11 % 10;
}

// Ireland: check letter from alphabet[(weighted digit sum + 9 * extra) mod 23].
// New style: 7 digits + check letter (+ optional trailing letter).
// Old style: digit + letter + 5 digits + check letter.
bool ChecksumIE(const std::string& digits) {
  static const std::string kAlphabet = "WABCDEFGHIJKLMNOPQRSTUV";
  auto calcCheck = [&](const std::string& seven, char extra) -> char {
    int total = 0;
    for (int i = 0; i < 7; i++) {
      if (!IsDigitChar(seven[i])) {
        return '\0';
      }
      total += (8 - i) * DigitValue(seven[i]);
    }
    if (extra != '\0') {
      auto pos = kAlphabet.find(extra);
      if (pos == std::string::npos) {
        return '\0';
      }
      total += 9 * static_cast<int>(pos);
    }
    return kAlphabet[total % 23];
  };
  if (digits.size() == 8 && !IsDigitChar(digits[1])) {
    // Old style: move the leading digit behind the body, pad to 7 digits.
    std::string seven = "0" + digits.substr(2, 5) + digits.substr(0, 1);
    return digits[7] == calcCheck(seven, '\0');
  }
  if (digits.size() == 8) {
    return digits[7] == calcCheck(digits.substr(0, 7), '\0');
  }
  if (digits.size() == 9) {
    return digits[7] == calcCheck(digits.substr(0, 7), digits[8]);
  }
  return false;
}

// Italy: 11 digits, Luhn-style (even 0-based positions plain, odd doubled).
bool ChecksumIT(const std::string& digits) {
  if (digits.size() != 11) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 11; i++) {
    int d = DigitValue(digits[i]);
    if (i % 2 == 1) {
      d *= 2;
      if (d > 9) {
        d -= 9;
      }
    }
    total += d;
  }
  return total % 10 == 0;
}

// Luxembourg: 8 digits; last two = first six mod 89.
bool ChecksumLU(const std::string& digits) {
  if (digits.size() != 8) {
    return false;
  }
  int64_t base = ToNumber(digits, 0, 6);
  int64_t check = ToNumber(digits, 6, 2);
  if (base < 0 || check < 0) {
    return false;
  }
  return check == base % 89;
}

// Netherlands: 9 digits + 'B' + 2 digits. Valid if the first 9 digits pass the
// legacy BSN mod-11 test, or (since 2020) the full number prefixed with 'NL'
// passes ISO 7064 MOD 97-10 (letters mapped A=10..Z=35, result == 1).
bool ChecksumNL(const std::string& digits) {
  if (digits.size() != 12 || digits[9] != 'B') {
    return false;
  }
  // Legacy mod-11 over the leading 9 digits.
  int total = 0;
  for (int i = 0; i < 8; i++) {
    total += (9 - i) * DigitValue(digits[i]);
  }
  int remainder = total % 11;
  if (remainder < 10 && remainder == DigitValue(digits[8])) {
    return true;
  }
  // MOD 97-10 over "NL" + number with letters converted to two-digit values.
  std::string full = "NL" + digits;
  int64_t mod = 0;
  for (char c : full) {
    if (IsDigitChar(c)) {
      mod = (mod * 10 + DigitValue(c)) % 97;
    } else {
      mod = (mod * 100 + (c - 'A' + 10)) % 97;
    }
  }
  return mod == 1;
}

// Poland: 10 digits, weights 6,5,7,2,3,4,5,6,7; check = total mod 11.
bool ChecksumPL(const std::string& digits) {
  static const int kWeights[] = {6, 5, 7, 2, 3, 4, 5, 6, 7};
  if (digits.size() != 10) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 9; i++) {
    total += kWeights[i] * DigitValue(digits[i]);
  }
  return DigitValue(digits[9]) == total % 11;
}

// Portugal: 9 digits, weights 9..2; check = (11 - total mod 11) mod 11 mod 10.
bool ChecksumPT(const std::string& digits) {
  if (digits.size() != 9) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 8; i++) {
    total += (9 - i) * DigitValue(digits[i]);
  }
  int check = (11 - total % 11) % 11 % 10;
  return DigitValue(digits[8]) == check;
}

// Sweden: 12 digits ending in 01; Luhn over the first 10 digits.
bool ChecksumSE(const std::string& digits) {
  if (digits.size() != 12) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 10; i++) {
    int d = DigitValue(digits[i]);
    if (i % 2 == 0) {
      d *= 2;
      if (d > 9) {
        d -= 9;
      }
    }
    total += d;
  }
  return total % 10 == 0;
}

// Slovenia: 8 digits, weights 8..2; check = (-total mod 11) mod 10.
bool ChecksumSI(const std::string& digits) {
  if (digits.size() != 8) {
    return false;
  }
  int total = 0;
  for (int i = 0; i < 7; i++) {
    total += (8 - i) * DigitValue(digits[i]);
  }
  int check = (11 - total % 11) % 11 % 10;
  return DigitValue(digits[7]) == check;
}

}  // namespace

bool VATRegistry::IsValidChecksum(const std::string& country,
                                  const std::string& digits) const {
  auto it = countries_.find(country);
  if (it == countries_.end()) {
    return false;
  }
  if (!it->second.has_checksum) {
    // No verified checksum algorithm implemented; syntax-only validation.
    return true;
  }
  if (country == "AT") return ChecksumAT(digits);
  if (country == "BE") return ChecksumBE(digits);
  if (country == "DE") return ChecksumDE(digits);
  if (country == "DK") return ChecksumDK(digits);
  if (country == "ES") return ChecksumES(digits);
  if (country == "FI") return ChecksumFI(digits);
  if (country == "FR") return ChecksumFR(digits);
  if (country == "GR") return ChecksumGR(digits);
  if (country == "IE") return ChecksumIE(digits);
  if (country == "IT") return ChecksumIT(digits);
  if (country == "LU") return ChecksumLU(digits);
  if (country == "NL") return ChecksumNL(digits);
  if (country == "PL") return ChecksumPL(digits);
  if (country == "PT") return ChecksumPT(digits);
  if (country == "SE") return ChecksumSE(digits);
  if (country == "SI") return ChecksumSI(digits);
  // has_checksum set without an implementation would be a programming error;
  // fail closed so the mismatch is caught by tests.
  return false;
}

}  // namespace duckdb
