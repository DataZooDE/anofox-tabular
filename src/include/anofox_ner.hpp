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
    NOT_AVAILABLE        // OpenVINO not compiled in
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

private:
    std::unordered_map<std::string, int> vocab_;

    static std::vector<std::string> Tokenize(const std::string &text);
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

private:
    std::unique_ptr<::sentencepiece::SentencePieceProcessor> processor_;

    // XLM-RoBERTa special tokens
    static constexpr int BOS_TOKEN_ID = 0;  // <s>
    static constexpr int EOS_TOKEN_ID = 2;  // </s>
    static constexpr int PAD_TOKEN_ID = 1;  // <pad>
};
#endif

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
    bool DownloadModel(const std::string &dest_path);
    std::vector<NEREntity> PostProcess(
        const std::vector<float> &logits,
        size_t seq_length,
        const std::string &text,
        const std::vector<std::pair<size_t, size_t>> &offsets
    );

    /**
     * Run cache lookup, tokenization, inference and post-processing for one
     * text. Shared by ExtractEntities and ExtractEntitiesBatch so the
     * thread-safety guarantees live in exactly one place.
     * Precondition: IsAvailable() returned true.
     */
    std::vector<NEREntity> RunSingleInference(const std::string &text);

    /**
     * Update status_message_ under state_mutex_
     */
    void SetStatusMessage(const std::string &message);

    /**
     * Compute the default on-disk model path (no member state accessed)
     */
    static std::string DefaultModelPath();

    std::atomic<NERStatus> status_{NERStatus::NOT_LOADED};
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
