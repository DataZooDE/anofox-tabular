#pragma once

#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <list>
#include <optional>
#include <stdexcept>
#include <utility>

#include "duckdb/main/extension/extension_loader.hpp"

#if HAVE_OPENVINO
#include <openvino/openvino.hpp>
#endif

// Forward declaration for SentencePiece (outside namespace)
#if HAVE_SENTENCEPIECE
namespace sentencepiece {
class SentencePieceProcessor;
}
#endif

namespace duckdb {
namespace anofox {

// ============================================================================
// LRU Cache for NER Results
// ============================================================================

/**
 * Thread-safe LRU (Least Recently Used) cache
 * Caches NER inference results to avoid redundant model calls
 */
template<typename Key, typename Value>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    /**
     * Get value from cache if present
     * @return optional containing value if found, empty otherwise
     */
    std::optional<Value> Get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }

        // Move to front (most recently used)
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);
        return it->second.first;
    }

    /**
     * Insert or update value in cache.
     * With capacity 0 the cache is disabled and Put is a safe no-op.
     */
    void Put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (capacity_ == 0) {
            // Cache disabled (e.g. SET anofox_ner_cache_size = 0). This must
            // be checked under the lock: callers cannot rely on a Capacity()
            // pre-check because another thread may shrink the capacity in
            // between (issue #50).
            return;
        }

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // Update existing entry
            it->second.first = value;
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);
            return;
        }

        // Evict if at capacity
        while (cache_.size() >= capacity_ && !lru_list_.empty()) {
            const Key& lru_key = lru_list_.back();
            cache_.erase(lru_key);
            lru_list_.pop_back();
        }

        // Insert new entry
        lru_list_.push_front(key);
        cache_[key] = {value, lru_list_.begin()};
    }

    /**
     * Clear all cached entries
     */
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_list_.clear();
    }

    /**
     * Get current cache size
     */
    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

    /**
     * Get cache capacity
     */
    size_t Capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    /**
     * Set new capacity (may evict entries)
     */
    void SetCapacity(size_t new_capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = new_capacity;

        // Evict excess entries
        while (cache_.size() > capacity_ && !lru_list_.empty()) {
            const Key& lru_key = lru_list_.back();
            cache_.erase(lru_key);
            lru_list_.pop_back();
        }
    }

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::list<Key> lru_list_;  // Front = most recent, back = least recent
    std::unordered_map<Key, std::pair<Value, typename std::list<Key>::iterator>> cache_;
};

/**
 * Entity detected by NER model
 */
struct NEREntity {
    std::string text;           // Matched text (e.g., "John Smith")
    std::string label;          // Entity type: PER, LOC, ORG, MISC
    size_t start_pos;           // Start byte offset
    size_t end_pos;             // End byte offset
    double confidence;          // Model probability score

    NEREntity() : start_pos(0), end_pos(0), confidence(0.0) {}

    NEREntity(const std::string &t, const std::string &l,
              size_t start, size_t end, double conf)
        : text(t), label(l), start_pos(start), end_pos(end), confidence(conf) {}
};

/**
 * NER model status
 */
enum class NERStatus {
    NOT_LOADED,          // Model not initialized
    DOWNLOADING,         // Download in progress
    LOADED,              // Ready for inference
    FAILED,              // Download/load failed
    NOT_AVAILABLE,       // OpenVINO not compiled in
    NOT_DOWNLOADED       // Assets missing and anofox_ner_auto_download is off
};

/**
 * Result of tokenizing a text: token ids plus the byte offsets of each token
 * in the original text. Returned by value so that concurrent Encode() calls
 * never share mutable tokenizer state (issue #50).
 */
struct TokenizedInput {
    std::vector<int64_t> ids;
    std::vector<std::pair<size_t, size_t>> offsets;
};

// ============================================================================
// Input bounding helpers (issue #56)
//
// Header-only so they can be unit-tested in anofox_tabular_cpp_tests without
// linking OpenVINO or the rest of the extension.
// ============================================================================

/**
 * Maximum number of tokens (including special tokens) fed into the NER model.
 * BERT-class models such as DistilBERT have max_position_embeddings = 512;
 * longer sequences fail shape inference inside OpenVINO. Longer inputs are
 * split into overlapping windows (NER_WINDOW_TOKENS, PlanNERWindows).
 */
constexpr size_t NER_MAX_SEQ_LENGTH = 512;

/**
 * Window length (in tokens, including the two special tokens) for inputs
 * longer than this. The model accepts 512 tokens, but the bundled
 * DistilBERT (trained on short CoNLL sentences) misses entities at late
 * positions: a name in the signature of a 4 KB ticket was not found with
 * ~450 tokens of preceding context, and was found with ~250. Windows of 256
 * tokens keep every entity in the range the model handles well.
 */
constexpr size_t NER_WINDOW_TOKENS = 256;

/**
 * Number of tokens two neighbouring windows share (see PlanNERWindows). Each
 * window owns the half of the overlap next to it, so every entity of up to
 * OVERLAP/2 tokens is seen in full, with context, by the window that reports
 * it.
 */
constexpr size_t NER_WINDOW_OVERLAP = 64;

/**
 * A window over the content tokens (the tokens between the leading and the
 * trailing special token) of a tokenized input: content tokens [begin, end)
 * are run through the model; entities starting in [own_begin, own_end) are
 * reported by this window.
 */
struct NERWindow {
    size_t begin;
    size_t end;
    size_t own_begin;
    size_t own_end;
};

/**
 * Split content_tokens tokens into windows of at most window_tokens tokens
 * that overlap by `overlap` tokens (the last window is aligned to the end of
 * the input, so it can overlap its neighbour by more). The owned ranges of the
 * windows partition [0, content_tokens): the boundary between two windows is
 * the middle of their overlap.
 *
 * An input that fits into one window yields exactly one window. window_tokens
 * of 0 means "unbounded" (one window).
 */
inline std::vector<NERWindow> PlanNERWindows(size_t content_tokens, size_t window_tokens, size_t overlap) {
    std::vector<NERWindow> windows;
    if (window_tokens == 0 || content_tokens <= window_tokens) {
        windows.push_back({0, content_tokens, 0, content_tokens});
        return windows;
    }
    // Keep the stride positive even for a nonsensical overlap
    const size_t stride = overlap < window_tokens ? window_tokens - overlap : window_tokens;
    for (size_t begin = 0;; begin += stride) {
        if (begin + window_tokens >= content_tokens) {
            windows.push_back({content_tokens - window_tokens, content_tokens, 0, 0});
            break;
        }
        windows.push_back({begin, begin + window_tokens, 0, 0});
    }
    for (size_t i = 0; i < windows.size(); ++i) {
        windows[i].own_begin = i == 0 ? 0 : (windows[i - 1].end + windows[i].begin) / 2;
        windows[i].own_end = i + 1 == windows.size() ? content_tokens : (windows[i].end + windows[i + 1].begin) / 2;
    }
    return windows;
}

/**
 * Build the model input of one window: the leading special token of `full`,
 * the window's content tokens and the trailing special token of `full`. The
 * byte offsets are those of the original text, so entities found in the
 * window map directly back to the whole input.
 * Precondition: full holds at least the two special tokens.
 */
inline TokenizedInput SliceTokenizedWindow(const TokenizedInput &full, const NERWindow &window) {
    TokenizedInput slice;
    const size_t count = window.end - window.begin;
    slice.ids.reserve(count + 2);
    slice.offsets.reserve(count + 2);
    slice.ids.push_back(full.ids.front());
    slice.offsets.push_back({0, 0});
    for (size_t i = window.begin; i < window.end; ++i) {
        // Content token i sits behind the leading special token
        slice.ids.push_back(full.ids[i + 1]);
        slice.offsets.push_back(full.offsets[i + 1]);
    }
    const size_t last_end = slice.offsets.back().second;
    slice.ids.push_back(full.ids.back());
    slice.offsets.push_back({last_end, last_end});
    return slice;
}

/**
 * Merge consecutive entities of the same type with no gap between them. This
 * repairs fragmentation where the model emits several B- tags instead of B-
 * followed by I- tags for one multi-token entity ("Don|au|wo|ert|h").
 * The merged entity has the mean confidence of its parts.
 * @param entities Entities sorted by start_pos
 */
inline std::vector<NEREntity> MergeConsecutiveSameTypeEntities(const std::vector<NEREntity> &entities) {
    if (entities.size() <= 1) {
        return entities;
    }
    std::vector<NEREntity> merged;
    merged.reserve(entities.size());
    size_t i = 0;
    while (i < entities.size()) {
        NEREntity current = entities[i];
        double total_confidence = current.confidence;
        int count = 1;
        while (i + 1 < entities.size() && entities[i + 1].label == current.label &&
               entities[i + 1].start_pos == current.end_pos) {
            i++;
            current.text += entities[i].text;
            current.end_pos = entities[i].end_pos;
            total_confidence += entities[i].confidence;
            count++;
        }
        current.confidence = total_confidence / count;
        merged.push_back(current);
        i++;
    }
    return merged;
}

/**
 * Combine the entities of all windows of one input into one list sorted by
 * position. Overlapping entities (an entity near a window boundary seen by
 * both windows) are reduced to one: entities of the same type are united
 * (outermost span, higher confidence), of different types the one with the
 * higher confidence is kept. Adjacent entities of the same type are then
 * merged (MergeConsecutiveSameTypeEntities). `text` is the whole input, used
 * to rebuild the text of a united span.
 */
inline std::vector<NEREntity> CombineWindowEntities(std::vector<NEREntity> entities, const std::string &text) {
    std::sort(entities.begin(), entities.end(), [](const NEREntity &a, const NEREntity &b) {
        if (a.start_pos != b.start_pos) {
            return a.start_pos < b.start_pos;
        }
        return a.end_pos > b.end_pos;
    });
    std::vector<NEREntity> combined;
    combined.reserve(entities.size());
    for (auto &entity : entities) {
        if (combined.empty() || entity.start_pos >= combined.back().end_pos) {
            combined.push_back(std::move(entity));
            continue;
        }
        auto &kept = combined.back();
        if (entity.label == kept.label) {
            if (entity.end_pos > kept.end_pos) {
                kept.end_pos = entity.end_pos;
                kept.text = text.substr(kept.start_pos, kept.end_pos - kept.start_pos);
            }
            kept.confidence = std::max(kept.confidence, entity.confidence);
        } else if (entity.confidence > kept.confidence) {
            kept = std::move(entity);
        }
    }
    return MergeConsecutiveSameTypeEntities(combined);
}

/**
 * Whether a text has natural-language structure, i.e. contains at least one
 * word: a maximal run of characters between separators (ASCII whitespace and
 * ASCII punctuation) that consists of letters only and has at least two of
 * them. ASCII letters and non-ASCII characters (multi-byte UTF-8, counted per
 * character) are letters; a digit makes the run no word.
 *
 * Values without a word (hex digests, UUIDs, numbers, base64 tokens with
 * digits, codes such as "A-12") carry no names for the NER model to find, so
 * NER skips them. "Li", "Wu", "Müller" and "J. Smith" are words.
 */
inline bool HasNaturalLanguageWord(const std::string &text) {
    size_t letters = 0;
    bool only_letters = true;
    for (size_t i = 0; i <= text.size(); ++i) {
        const unsigned char c = i < text.size() ? static_cast<unsigned char>(text[i]) : ' ';
        if (c < 0x80 && (std::isspace(c) || std::ispunct(c))) {
            if (only_letters && letters >= 2) {
                return true;
            }
            letters = 0;
            only_letters = true;
        } else if (c >= 0x80) {
            // Count each UTF-8 character once (skip continuation bytes)
            if ((c & 0xC0) != 0x80) {
                letters++;
            }
        } else if (std::isalpha(c)) {
            letters++;
        } else {
            only_letters = false;
        }
    }
    return false;
}

/**
 * Validate the shape of the model output tensor before reading any logits.
 *
 * The NER model must emit exactly seq_length * num_labels logits, laid out as
 * [1, seq_length, num_labels] (or the squeezed [seq_length, num_labels]).
 * Reading a differently shaped buffer would be out-of-bounds, so a mismatch
 * raises a clear std::runtime_error instead.
 */
inline void ValidateLogitsShape(const std::vector<size_t> &shape, size_t seq_length,
                                size_t num_labels) {
    size_t total_elements = shape.empty() ? 0 : 1;
    for (size_t dim : shape) {
        total_elements *= dim;
    }
    const bool valid = !shape.empty() && shape.back() == num_labels &&
                       total_elements == seq_length * num_labels;
    if (!valid) {
        std::string actual;
        for (size_t i = 0; i < shape.size(); ++i) {
            if (i > 0) {
                actual += "x";
            }
            actual += std::to_string(shape[i]);
        }
        throw std::runtime_error(
            "NER output tensor shape mismatch: got [" + actual + "], expected " +
            std::to_string(seq_length) + "x" + std::to_string(num_labels) +
            " logits (1x" + std::to_string(seq_length) + "x" + std::to_string(num_labels) + ")");
    }
}

/**
 * Character class of one code point for BasicWordSplit, following the BERT
 * pre-tokenizer (BertPreTokenizer / BasicTokenizer of the reference
 * implementation).
 */
enum class NERCharClass { WORD, SPACE, PUNCT, IDEOGRAPH };

/**
 * Classify a Unicode code point the way the BERT pre-tokenizer does: ASCII
 * punctuation and the Unicode punctuation blocks used by European and the
 * other supported languages ("„", "“", "«", "–", "…", "¿", "、") are
 * punctuation; Unicode spaces (no-break, thin, ideographic) and format
 * characters (zero-width space, soft hyphen) separate words; CJK ideographs
 * are words of their own.
 */
inline NERCharClass ClassifyNERCodePoint(uint32_t cp) {
    if (cp < 0x80) {
        const auto c = static_cast<unsigned char>(cp);
        if (std::isspace(c) || std::iscntrl(c)) {
            return NERCharClass::SPACE;
        }
        return std::ispunct(c) ? NERCharClass::PUNCT : NERCharClass::WORD;
    }
    // Spaces (Zs) and format characters (Cf)
    if (cp == 0x00A0 || cp == 0x00AD || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F) ||
        (cp >= 0x2028 && cp <= 0x202F) || cp == 0x205F || (cp >= 0x2060 && cp <= 0x2064) ||
        cp == 0x3000 || cp == 0xFEFF) {
        return NERCharClass::SPACE;
    }
    // Punctuation (P*): Latin-1, Greek, Armenian, Hebrew, Arabic, General
    // Punctuation (without the math symbols U+2044 and U+2052), CJK and
    // fullwidth forms
    if (cp == 0x00A1 || cp == 0x00A7 || cp == 0x00AB || cp == 0x00B6 || cp == 0x00B7 || cp == 0x00BB ||
        cp == 0x00BF || cp == 0x037E || cp == 0x0387 || (cp >= 0x055A && cp <= 0x055F) || cp == 0x0589 ||
        cp == 0x058A || cp == 0x05BE || cp == 0x05C0 || cp == 0x05C3 || cp == 0x05C6 || cp == 0x05F3 ||
        cp == 0x05F4 || cp == 0x060C || cp == 0x060D || cp == 0x061B || cp == 0x061F ||
        (cp >= 0x066A && cp <= 0x066D) || cp == 0x06D4 || (cp >= 0x2010 && cp <= 0x2027) ||
        (cp >= 0x2030 && cp <= 0x2043) || (cp >= 0x2045 && cp <= 0x2051) || (cp >= 0x2053 && cp <= 0x205E) ||
        (cp >= 0x2E00 && cp <= 0x2E4F) || (cp >= 0x3001 && cp <= 0x3003) || (cp >= 0x3008 && cp <= 0x3011) ||
        (cp >= 0x3014 && cp <= 0x301F) || cp == 0x3030 || cp == 0x303D || cp == 0x30FB ||
        (cp >= 0xFE10 && cp <= 0xFE19) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFE50 && cp <= 0xFE6B) ||
        (cp >= 0xFF01 && cp <= 0xFF03) || (cp >= 0xFF05 && cp <= 0xFF0A) || (cp >= 0xFF0C && cp <= 0xFF0F) ||
        cp == 0xFF1A || cp == 0xFF1B || cp == 0xFF1F || cp == 0xFF20 || (cp >= 0xFF3B && cp <= 0xFF3D) ||
        cp == 0xFF3F || cp == 0xFF5B || cp == 0xFF5D || (cp >= 0xFF5F && cp <= 0xFF65)) {
        return NERCharClass::PUNCT;
    }
    // CJK ideographs (the ranges of the reference _is_chinese_char)
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x20000 && cp <= 0x2A6DF) ||
        (cp >= 0x2A700 && cp <= 0x2B73F) || (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F)) {
        return NERCharClass::IDEOGRAPH;
    }
    return NERCharClass::WORD;
}

/**
 * Length in bytes of the UTF-8 sequence starting at text[i] and its code
 * point. An invalid or truncated sequence is one byte, classified as a word
 * character, so no input byte is ever dropped.
 */
inline size_t DecodeUTF8CodePoint(const std::string &text, size_t i, uint32_t &cp) {
    const auto lead = static_cast<unsigned char>(text[i]);
    size_t length = 1;
    if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        cp = lead & 0x07;
    } else if (lead >= 0xE0) {
        length = 3;
        cp = lead & 0x0F;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        cp = lead & 0x1F;
    } else {
        cp = lead < 0x80 ? lead : 0xFFFD;
        return 1;
    }
    if (lead > 0xF4 || i + length > text.size()) {
        cp = 0xFFFD;
        return 1;
    }
    for (size_t k = 1; k < length; ++k) {
        const auto c = static_cast<unsigned char>(text[i + k]);
        if ((c & 0xC0) != 0x80) {
            cp = 0xFFFD;
            return 1;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    return length;
}

/**
 * Word splitting used by the hand-written WordPiece tokenizer, following the
 * BERT pre-tokenizer: split on whitespace (ASCII and Unicode spaces), keep
 * every punctuation character (ASCII and Unicode, see ClassifyNERCodePoint)
 * and every CJK ideograph as a word of its own. UTF-8 sequences are never
 * split apart. The multilingual models need the Unicode punctuation: German
 * quotes („Sabine Weiß“) would otherwise glue to the name.
 */
inline std::vector<std::string> BasicWordSplit(const std::string &text) {
    std::vector<std::string> words;
    std::string current_word;

    size_t i = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        const size_t length = DecodeUTF8CodePoint(text, i, cp);
        const auto char_class = ClassifyNERCodePoint(cp);
        if (char_class == NERCharClass::WORD) {
            current_word.append(text, i, length);
        } else {
            if (!current_word.empty()) {
                words.push_back(current_word);
                current_word.clear();
            }
            if (char_class != NERCharClass::SPACE) {
                words.push_back(text.substr(i, length));
            }
        }
        i += length;
    }
    if (!current_word.empty()) {
        words.push_back(current_word);
    }

    return words;
}

/**
 * Abstract base class for tokenizers.
 * Encode() is const and returns all per-call state by value, so a loaded
 * tokenizer can be shared across DuckDB worker threads.
 */
class ITokenizer {
public:
    virtual ~ITokenizer() = default;
    virtual bool Load(const std::string &model_path) = 0;
    virtual TokenizedInput Encode(const std::string &text) const = 0;
    virtual bool IsLoaded() const = 0;

    /**
     * Id of the special token that terminates a sequence ([SEP] / </s>).
     */
    virtual int64_t SequenceEndTokenId() const = 0;
};

/**
 * WordPiece tokenizer for DistilBERT
 */
class WordPieceTokenizer : public ITokenizer {
public:
    WordPieceTokenizer();
    ~WordPieceTokenizer() override;

    /**
     * Load vocabulary from tokenizer.json
     */
    bool Load(const std::string &vocab_path) override;
    bool LoadVocab(const std::string &vocab_path) { return Load(vocab_path); }

    /**
     * Encode text to token IDs and byte offsets (returned by value)
     */
    TokenizedInput Encode(const std::string &text) const override;

    /**
     * Check if vocabulary is loaded
     */
    bool IsLoaded() const override { return !vocab_.empty(); }

    /**
     * BERT/DistilBERT [SEP] token id
     */
    int64_t SequenceEndTokenId() const override;

private:
    std::unordered_map<std::string, int> vocab_;
    //! Words longer than this (in characters) become one [UNK] token, as in
    //! the reference WordPiece tokenizer ("max_input_chars_per_word", 100)
    size_t max_input_chars_per_word_ = 100;

    int TokenToId(const std::string &token) const;
};

#if HAVE_SENTENCEPIECE
/**
 * SentencePiece tokenizer for XLM-RoBERTa (multilingual NER)
 */
class SentencePieceTokenizer : public ITokenizer {
public:
    SentencePieceTokenizer();
    ~SentencePieceTokenizer() override;

    /**
     * Load SentencePiece model file (.model or sentencepiece.bpe.model)
     */
    bool Load(const std::string &model_path) override;

    /**
     * Encode text to token IDs and byte offsets (returned by value)
     */
    TokenizedInput Encode(const std::string &text) const override;

    /**
     * Check if model is loaded
     */
    bool IsLoaded() const override;

    /**
     * XLM-RoBERTa </s> (EOS) token id
     */
    int64_t SequenceEndTokenId() const override { return EOS_TOKEN_ID; }

private:
    std::unique_ptr<::sentencepiece::SentencePieceProcessor> processor_;

    // XLM-RoBERTa special tokens
    static constexpr int BOS_TOKEN_ID = 0;  // <s>
    static constexpr int EOS_TOKEN_ID = 2;  // </s>
    static constexpr int PAD_TOKEN_ID = 1;  // <pad>
};
#endif

/**
 * Metadata describing a supported NER model (defined in anofox_ner.cpp).
 * Drives on-disk asset paths and download URLs (issue #56).
 */
struct ModelMetadata;

/**
 * Immutable snapshot of the NER model manager status.
 * Returned by value under a mutex so status table functions never observe
 * torn strings while a loader thread updates the fields (issue #50).
 */
struct NERStatusSnapshot {
    NERStatus status = NERStatus::NOT_LOADED;
    std::string message;
    std::string model_path;
    std::string model_name;
    std::string device;
};

/**
 * Singleton OpenVINO NER Model Manager
 *
 * Responsibilities:
 * - Lazy-load pre-trained model on first use
 * - Download model from HuggingFace if not cached locally
 * - Thread-safe initialization
 * - Provide entity extraction interface using OpenVINO
 *
 * Thread safety: after LoadModel() publishes NERStatus::LOADED, tokenizer_
 * and compiled_model_ are treated as immutable. Each inference call creates
 * its own ov::InferRequest (compiled models are thread-safe for creating
 * requests), and tokenization returns its state by value, so ExtractEntities
 * can be called concurrently from DuckDB worker threads.
 */
class NERModelManager {
public:
    static NERModelManager& Instance();

    // Default cache capacity for LRU cache
    static constexpr size_t DEFAULT_CACHE_SIZE = 10000;

    /**
     * Ensure model is loaded (downloads on first use)
     */
    void EnsureInitialized();

    /**
     * Extract entities from text
     * @return Vector of entities (PER, LOC, ORG, MISC)
     */
    std::vector<NEREntity> ExtractEntities(const std::string &text);

    /**
     * Extract entities from multiple texts.
     * Deduplicates the inputs and shares the result cache with
     * ExtractEntities; inference itself runs one text at a time.
     * @param texts Vector of input texts
     * @return Vector of entity vectors (one per input text)
     */
    std::vector<std::vector<NEREntity>> ExtractEntitiesBatch(
        const std::vector<std::string> &texts
    );

    /**
     * Check if model is ready for inference
     */
    bool IsAvailable() const;

    /**
     * Get cache statistics
     */
    size_t GetCacheSize() const;
    size_t GetCacheCapacity() const;

    /**
     * Set cache capacity (0 = disabled)
     */
    void SetCacheCapacity(size_t capacity);

    /**
     * Clear the result cache
     */
    void ClearCache();

    /**
     * Set OpenVINO inference device (e.g. "AUTO", "CPU", "GPU", "GPU.0")
     * Takes effect on next model load
     */
    void SetDevice(const std::string &device_name);

    /**
     * Get current configured device name
     */
    std::string GetDevice() const;

    /**
     * Allow or forbid downloading missing model assets from Hugging Face.
     * Turning it back on lets the next NER call retry a skipped download.
     */
    void SetAutoDownload(bool enabled);

    /**
     * Get list of devices available to OpenVINO at runtime
     */
    std::vector<std::string> GetAvailableDevices() const;

    /**
     * Get current model status
     */
    NERStatus GetStatus() const { return status_.load(); }

    /**
     * Get a consistent snapshot of status, message, model path/name and device
     */
    NERStatusSnapshot GetStatusSnapshot() const;

    /**
     * Get status message for display
     */
    std::string GetStatusMessage() const;

    /**
     * Get model file path
     */
    std::string GetModelPath() const;

    /**
     * Get model size in MB (0 if not loaded)
     */
    double GetModelSizeMB() const;

private:
    NERModelManager();
    ~NERModelManager() = default;
    NERModelManager(const NERModelManager&) = delete;
    NERModelManager& operator=(const NERModelManager&) = delete;

    void LoadModel();

    /**
     * Download the model + tokenizer asset pair described by `metadata`.
     * Files are first written to .tmp paths and atomically renamed on
     * success, so partial downloads never leave mismatched pairs (issue #56).
     */
    bool DownloadModel(const ModelMetadata &metadata, const std::string &model_dest,
                       const std::string &tokenizer_dest);
    std::vector<NEREntity> PostProcess(
        const std::vector<float> &logits,
        size_t seq_length,
        const std::string &text,
        const std::vector<std::pair<size_t, size_t>> &offsets
    );

    /**
     * Run cache lookup, tokenization, inference and post-processing for one
     * text. Shared by ExtractEntities and ExtractEntitiesBatch so the
     * thread-safety guarantees live in exactly one place. A text longer than
     * NER_WINDOW_TOKENS tokens is split into overlapping windows
     * (PlanNERWindows); their entities are combined (CombineWindowEntities).
     * Precondition: IsAvailable() returned true.
     */
    std::vector<NEREntity> RunSingleInference(const std::string &text);

    /**
     * Run the model on one tokenized sequence of at most max_seq_length_
     * tokens and return its entities with byte offsets into `text`.
     */
    std::vector<NEREntity> InferSequence(TokenizedInput tokenized, const std::string &text);

    /**
     * Update status_message_ under state_mutex_
     */
    void SetStatusMessage(const std::string &message);

    /**
     * Compute the default on-disk model path (no member state accessed)
     */
    static std::string DefaultModelPath();

    std::atomic<NERStatus> status_{NERStatus::NOT_LOADED};
    std::atomic<bool> auto_download_{true};
    std::mutex init_lock_;

    // Guards status_message_, model_path_, current_model_name_ and
    // device_name_ against torn reads from status table functions while a
    // loader thread updates them (issue #50)
    mutable std::mutex state_mutex_;
    std::string status_message_;
    std::string model_path_;
    std::string device_name_ = "AUTO";

#if HAVE_OPENVINO
    std::shared_ptr<ov::Core> core_;
    // Immutable after LoadModel() publishes NERStatus::LOADED; inference
    // requests are created per call (ov::InferRequest is stateful and must
    // not be shared across threads)
    std::shared_ptr<ov::CompiledModel> compiled_model_;
#endif

    // Tokenizer (WordPiece for DistilBERT, SentencePiece for XLM-RoBERTa);
    // immutable after LoadModel() publishes NERStatus::LOADED
    std::unique_ptr<ITokenizer> tokenizer_;
    std::string current_model_name_;  // Track which model is loaded (guarded by state_mutex_)

    // Maximum sequence length of the loaded model (from ModelMetadata);
    // written by LoadModel() before LOADED is published, immutable afterwards
    size_t max_seq_length_ = NER_MAX_SEQ_LENGTH;

    // Result cache for repeated texts
    std::unique_ptr<LRUCache<std::string, std::vector<NEREntity>>> result_cache_;

    // DistilBERT NER labels: O, B-PER, I-PER, B-LOC, I-LOC, B-ORG, I-ORG, B-MISC, I-MISC
    static constexpr int NUM_LABELS = 9;
    static const std::vector<std::string>& GetLabels();
};

/**
 * Register NER configuration options with DuckDB
 */
void RegisterNEROptions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
