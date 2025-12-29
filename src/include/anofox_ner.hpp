#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <list>
#include <optional>

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
     * Insert or update value in cache
     */
    void Put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // Update existing entry
            it->second.first = value;
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second.second);
            return;
        }

        // Evict if at capacity
        if (cache_.size() >= capacity_) {
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
    size_t Capacity() const { return capacity_; }

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
    NOT_AVAILABLE        // OpenVINO not compiled in
};

/**
 * Abstract base class for tokenizers
 */
class ITokenizer {
public:
    virtual ~ITokenizer() = default;
    virtual bool Load(const std::string &model_path) = 0;
    virtual std::vector<int64_t> Encode(const std::string &text) = 0;
    virtual const std::vector<std::pair<size_t, size_t>>& GetOffsets() const = 0;
    virtual bool IsLoaded() const = 0;
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
     * Encode text to token IDs
     */
    std::vector<int64_t> Encode(const std::string &text) override;

    /**
     * Get byte offsets for each token (for mapping back to original text)
     */
    const std::vector<std::pair<size_t, size_t>>& GetOffsets() const override { return offsets_; }

    /**
     * Check if vocabulary is loaded
     */
    bool IsLoaded() const override { return !vocab_.empty(); }

private:
    std::unordered_map<std::string, int> vocab_;
    std::vector<std::pair<size_t, size_t>> offsets_;

    std::vector<std::string> Tokenize(const std::string &text);
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
     * Encode text to token IDs
     */
    std::vector<int64_t> Encode(const std::string &text) override;

    /**
     * Get byte offsets for each token (for mapping back to original text)
     */
    const std::vector<std::pair<size_t, size_t>>& GetOffsets() const override { return offsets_; }

    /**
     * Check if model is loaded
     */
    bool IsLoaded() const override;

private:
    std::unique_ptr<::sentencepiece::SentencePieceProcessor> processor_;
    std::vector<std::pair<size_t, size_t>> offsets_;

    // XLM-RoBERTa special tokens
    static constexpr int BOS_TOKEN_ID = 0;  // <s>
    static constexpr int EOS_TOKEN_ID = 2;  // </s>
    static constexpr int PAD_TOKEN_ID = 1;  // <pad>
};
#endif

/**
 * Singleton OpenVINO NER Model Manager
 *
 * Responsibilities:
 * - Lazy-load pre-trained model on first use
 * - Download model from HuggingFace if not cached locally
 * - Thread-safe initialization
 * - Provide entity extraction interface using OpenVINO
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
     * Extract entities from multiple texts (batch processing)
     * More efficient than calling ExtractEntities for each text
     * @param texts Vector of input texts
     * @param max_batch_size Maximum texts to process in single inference
     * @return Vector of entity vectors (one per input text)
     */
    std::vector<std::vector<NEREntity>> ExtractEntitiesBatch(
        const std::vector<std::string> &texts,
        size_t max_batch_size = 32
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
     * Get current model status
     */
    NERStatus GetStatus() const { return status_.load(); }

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
    bool DownloadModel(const std::string &dest_path);
    std::vector<NEREntity> PostProcess(
        const std::vector<float> &logits,
        size_t seq_length,
        const std::string &text,
        const std::vector<std::pair<size_t, size_t>> &offsets
    );

    std::atomic<NERStatus> status_{NERStatus::NOT_LOADED};
    std::mutex init_lock_;
    std::string status_message_;
    std::string model_path_;

#if HAVE_OPENVINO
    std::shared_ptr<ov::Core> core_;
    std::shared_ptr<ov::CompiledModel> compiled_model_;
    std::shared_ptr<ov::InferRequest> infer_request_;
#endif

    // Tokenizer (WordPiece for DistilBERT, SentencePiece for XLM-RoBERTa)
    std::unique_ptr<ITokenizer> tokenizer_;
    std::string current_model_name_;  // Track which model is loaded

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
