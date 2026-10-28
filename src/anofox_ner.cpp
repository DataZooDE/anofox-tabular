#include "anofox_ner.hpp"
#include "anofox_raii.hpp"
#include "anofox_curl_ca.hpp"
#include "anofox_trace.hpp"
#include "yyjson.hpp"
#include "duckdb.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cctype>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <curl/curl.h>

#if HAVE_SENTENCEPIECE
#include <sentencepiece_processor.h>
#include <sentencepiece.pb.h>
#endif

using namespace duckdb_yyjson;

// Callback function for curl to write downloaded data to file
static size_t WriteDataToFile(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

namespace duckdb {
namespace anofox {

// ============================================================================
// Model Metadata Registry
// ============================================================================

/**
 * Metadata for supported NER models.
 *
 * All on-disk asset paths and download URLs are derived from this registry
 * (one directory per model name), so a model may only be listed here if its
 * assets are actually downloadable (issue #56). The previously advertised
 * "xlm-roberta-multi" entry was removed: its HuggingFace URLs do not exist
 * (HTTP 401, the repository has no ONNX export) and SentencePiece support is
 * disabled in the build, so selecting it silently ran DistilBERT instead.
 */
struct ModelMetadata {
    std::string name;            // Short identifier (e.g., "distilbert-en")
    std::string display_name;    // Human-readable name
    std::string licence;         // Licence of the model weights (SPDX-like)
    std::string model_url;       // URL to download ONNX model
    std::string tokenizer_url;   // URL to download tokenizer config
    std::string model_file;      // On-disk model file name inside the model directory
    std::string tokenizer_file;  // On-disk tokenizer file name inside the model directory
    std::string tokenizer_type;  // "wordpiece" or "sentencepiece"
    std::vector<std::string> languages;  // Supported languages
    size_t model_size_mb;        // Approximate model size in MB
    size_t max_seq_length;       // Maximum input sequence length (incl. special tokens)
    // Output labels in model order (id2label of the model's config.json).
    // B-/I- labels of the types PER, ORG, LOC and MISC become entities; other
    // types (DATE of the multilingual models) are read as O, see
    // IsReportedNEREntityType
    std::vector<std::string> labels;
};

/**
 * Get list of all supported NER models
 */
static const std::vector<ModelMetadata>& GetSupportedModels() {
    // CoNLL-2003 (dslim/distilbert-NER, elastic/distilbert-base-cased-finetuned-conll03-english)
    static const std::vector<std::string> conll03_labels = {"O",     "B-PER", "I-PER", "B-ORG", "I-ORG",
                                                     "B-LOC", "I-LOC", "B-MISC", "I-MISC"};
    // Davlan *-multilingual-cased-ner-hrl (10 high-resource languages)
    static const std::vector<std::string> hrl_labels = {"O",     "B-DATE", "I-DATE", "B-PER", "I-PER",
                                                 "B-ORG", "I-ORG",  "B-LOC",  "I-LOC"};
    static std::vector<ModelMetadata> models = {
        {
            "distilbert-en",
            "DistilBERT English, CoNLL-2003",
            "Apache-2.0",
            "https://huggingface.co/onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX/resolve/main/onnx/model_quantized.onnx",
            "https://huggingface.co/onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX/resolve/main/tokenizer.json",
            "model_quantized.onnx",
            "tokenizer.json",
            "wordpiece",
            {"en"},
            66,
            NER_MAX_SEQ_LENGTH,
            conll03_labels
        },
        {
            "distilbert-multi",
            "DistilBERT multilingual, Davlan NER-HRL",
            "AFL-3.0",
            "https://huggingface.co/Xenova/distilbert-base-multilingual-cased-ner-hrl/resolve/main/onnx/model_quantized.onnx",
            "https://huggingface.co/Xenova/distilbert-base-multilingual-cased-ner-hrl/resolve/main/tokenizer.json",
            "model_quantized.onnx",
            "tokenizer.json",
            "wordpiece",
            {"ar", "de", "en", "es", "fr", "it", "lv", "nl", "pt", "zh"},
            135,
            NER_MAX_SEQ_LENGTH,
            hrl_labels
        },
        {
            "bert-multi",
            "BERT-base multilingual, Davlan NER-HRL",
            "AFL-3.0",
            "https://huggingface.co/Xenova/bert-base-multilingual-cased-ner-hrl/resolve/main/onnx/model_quantized.onnx",
            "https://huggingface.co/Xenova/bert-base-multilingual-cased-ner-hrl/resolve/main/tokenizer.json",
            "model_quantized.onnx",
            "tokenizer.json",
            "wordpiece",
            {"ar", "de", "en", "es", "fr", "it", "lv", "nl", "pt", "zh"},
            178,
            NER_MAX_SEQ_LENGTH,
            hrl_labels
        }
    };
    return models;
}

/**
 * Get metadata for a specific model by name
 * @return Pointer to metadata, or nullptr if not found
 */
static const ModelMetadata* GetModelMetadata(const std::string& model_name) {
    auto& models = GetSupportedModels();
    for (const auto& model : models) {
        if (model.name == model_name) {
            return &model;
        }
    }
    return nullptr;
}

/**
 * Get list of valid model names (for error messages)
 */
static std::string GetValidModelNames() {
    std::string result;
    auto& models = GetSupportedModels();
    for (size_t i = 0; i < models.size(); i++) {
        if (i > 0) result += ", ";
        result += models[i].name;
    }
    return result;
}

/**
 * One-line description of every model for the anofox_ner_model setting:
 * "distilbert-en (en, 66 MB, Apache-2.0), ..."
 */
static std::string DescribeSupportedModels() {
    std::string result;
    for (const auto &model : GetSupportedModels()) {
        if (!result.empty()) {
            result += "; ";
        }
        std::string languages;
        for (const auto &language : model.languages) {
            languages += (languages.empty() ? "" : " ") + language;
        }
        result += model.name + " (" + languages + ", " + std::to_string(model.model_size_mb) + " MB, " +
                  model.licence + ")";
    }
    return result;
}

// Current model configuration (thread-safe singleton)
static std::string g_current_model_name = NER_DEFAULT_MODEL;
static std::mutex g_model_config_mutex;

static std::string GetCurrentModelName() {
    std::lock_guard<std::mutex> lock(g_model_config_mutex);
    return g_current_model_name;
}

static void SetCurrentModelName(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_model_config_mutex);
    g_current_model_name = name;
}

// ============================================================================
// WordPieceTokenizer Implementation
// ============================================================================

// Special token IDs for BERT/DistilBERT
static constexpr int CLS_TOKEN_ID = 101;
static constexpr int SEP_TOKEN_ID = 102;
static constexpr int UNK_TOKEN_ID = 100;

WordPieceTokenizer::WordPieceTokenizer() = default;
WordPieceTokenizer::~WordPieceTokenizer() = default;

bool WordPieceTokenizer::Load(const std::string &vocab_path) {
    AnofoxTrace(AnofoxLogLevel::Debug, "ner: Loading tokenizer vocabulary from: " + vocab_path);

    // Read the tokenizer.json file
    std::ifstream file(vocab_path);
    if (!file.is_open()) {
        AnofoxTrace(AnofoxLogLevel::Warn, "ner: Cannot open tokenizer file: " + vocab_path);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json_str = buffer.str();
    file.close();

    // Parse JSON using yyjson; the RAII guard frees the document on every
    // exit path (issue #60).
    struct YyjsonDocFree {
        void operator()(yyjson_doc *doc_handle) const {
            yyjson_doc_free(doc_handle);
        }
    };
    UniqueHandle<yyjson_doc *, YyjsonDocFree, nullptr> doc(yyjson_read(json_str.c_str(), json_str.size(), 0));
    if (!doc) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Failed to parse tokenizer JSON");
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc.Get());
    if (!root) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Empty tokenizer JSON");
        return false;
    }

    // Navigate to model.vocab
    yyjson_val *model = yyjson_obj_get(root, "model");
    if (!model) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: No 'model' key in tokenizer JSON");
        return false;
    }

    yyjson_val *vocab = yyjson_obj_get(model, "vocab");
    if (!vocab) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: No 'model.vocab' key in tokenizer JSON");
        return false;
    }

    yyjson_val *max_chars = yyjson_obj_get(model, "max_input_chars_per_word");
    if (max_chars && yyjson_is_int(max_chars) && yyjson_get_int(max_chars) > 0) {
        max_input_chars_per_word_ = static_cast<size_t>(yyjson_get_int(max_chars));
    }

    // Iterate over vocabulary entries
    vocab_.clear();
    size_t idx, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(vocab, idx, max, key, value) {
        const char *token_str = yyjson_get_str(key);
        if (token_str && yyjson_is_int(value)) {
            int token_id = static_cast<int>(yyjson_get_int(value));
            vocab_[std::string(token_str)] = token_id;
        }
    }

    AnofoxTrace(AnofoxLogLevel::Info, "ner: Loaded vocabulary with " + std::to_string(vocab_.size()) + " tokens");
    return !vocab_.empty();
}

int64_t WordPieceTokenizer::SequenceEndTokenId() const {
    return SEP_TOKEN_ID;
}

int WordPieceTokenizer::TokenToId(const std::string &token) const {
    auto it = vocab_.find(token);
    if (it != vocab_.end()) {
        return it->second;
    }
    return UNK_TOKEN_ID;
}

TokenizedInput WordPieceTokenizer::Encode(const std::string &text) const {
    // All per-call state lives in `result`; this method only reads the shared
    // vocabulary, so concurrent Encode() calls are safe (issue #50)
    TokenizedInput result;

    if (vocab_.empty()) {
        AnofoxTrace(AnofoxLogLevel::Warn, "ner: Vocabulary not loaded, cannot encode");
        return result;
    }

    // Add [CLS] token
    result.ids.push_back(CLS_TOKEN_ID);
    result.offsets.push_back({0, 0});  // CLS has no text position

    // Tokenize text into words (ASCII-focused splitting, see BasicWordSplit)
    std::vector<std::string> words = BasicWordSplit(text);

    // Track position in original text
    size_t text_pos = 0;

    for (const auto &word : words) {
        // Find word position in original text
        size_t word_start = text.find(word, text_pos);
        if (word_start == std::string::npos) {
            word_start = text_pos;  // Fallback
        }
        text_pos = word_start + word.size();

        // A word longer than max_input_chars_per_word is one [UNK], as in the
        // reference tokenizer. Without the limit the longest-match search
        // below is cubic in the word length: a base64 attachment or a long
        // token without spaces took minutes.
        size_t char_count = 0;
        for (unsigned char c : word) {
            char_count += (c & 0xC0) != 0x80 ? 1 : 0;
        }
        if (char_count > max_input_chars_per_word_) {
            result.ids.push_back(UNK_TOKEN_ID);
            result.offsets.push_back({word_start, word_start + word.size()});
            continue;
        }

        // Apply WordPiece tokenization to each word
        size_t start = 0;
        bool first_subword = true;

        while (start < word.size()) {
            std::string prefix = first_subword ? "" : "##";
            size_t end = word.size();
            bool found = false;

            // Try to find longest matching subword in vocabulary
            while (end > start) {
                std::string subword = prefix + word.substr(start, end - start);
                auto it = vocab_.find(subword);
                if (it != vocab_.end()) {
                    result.ids.push_back(it->second);
                    // Calculate byte offsets for this subword
                    size_t byte_start = word_start + start;
                    size_t byte_end = word_start + end;
                    result.offsets.push_back({byte_start, byte_end});
                    start = end;
                    found = true;
                    break;
                }
                --end;
            }

            if (!found) {
                // No matching subword found, use [UNK]
                result.ids.push_back(UNK_TOKEN_ID);
                result.offsets.push_back({word_start + start, word_start + start + 1});
                ++start;
            }

            first_subword = false;
        }
    }

    // Add [SEP] token
    result.ids.push_back(SEP_TOKEN_ID);
    result.offsets.push_back({text.size(), text.size()});  // SEP has no text position

    return result;
}

// ============================================================================
// SentencePieceTokenizer Implementation
// ============================================================================

#if HAVE_SENTENCEPIECE

SentencePieceTokenizer::SentencePieceTokenizer()
    : processor_(std::make_unique<::sentencepiece::SentencePieceProcessor>()) {
}

SentencePieceTokenizer::~SentencePieceTokenizer() = default;

bool SentencePieceTokenizer::Load(const std::string &model_path) {
    AnofoxTrace(AnofoxLogLevel::Debug, "ner: Loading SentencePiece model from: " + model_path);

    auto status = processor_->Load(model_path);
    if (!status.ok()) {
        AnofoxTrace(AnofoxLogLevel::Error,
                    "ner: Failed to load SentencePiece model: " + std::string(status.message()));
        return false;
    }

    AnofoxTrace(AnofoxLogLevel::Info,
                "ner: Loaded SentencePiece model with " +
                std::to_string(processor_->GetPieceSize()) + " tokens");
    return true;
}

bool SentencePieceTokenizer::IsLoaded() const {
    return processor_ && processor_->GetPieceSize() > 0;
}

TokenizedInput SentencePieceTokenizer::Encode(const std::string &text) const {
    // All per-call state lives in `result`; the SentencePiece processor is
    // only read, so concurrent Encode() calls are safe (issue #50)
    TokenizedInput result;

    if (!IsLoaded() || text.empty()) {
        return result;
    }

    // Tokenize using SentencePiece
    std::vector<int> piece_ids;
    auto status = processor_->Encode(text, &piece_ids);
    if (!status.ok()) {
        AnofoxTrace(AnofoxLogLevel::Error,
                    "ner: SentencePiece encoding error: " + std::string(status.message()));
        return result;
    }

    // Convert to int64_t and add special tokens
    // XLM-RoBERTa format: <s> tokens </s>
    result.ids.reserve(piece_ids.size() + 2);
    result.offsets.reserve(piece_ids.size() + 2);

    // Add <s> (BOS) token
    result.ids.push_back(BOS_TOKEN_ID);
    result.offsets.push_back(std::make_pair(size_t(0), size_t(0)));  // BOS has no text position

    // Get pieces with their byte offsets
    ::sentencepiece::SentencePieceText spt;
    status = processor_->Encode(text, &spt);
    if (!status.ok()) {
        // Fallback: use the piece_ids without offsets
        for (int pid : piece_ids) {
            result.ids.push_back(static_cast<int64_t>(pid));
            result.offsets.push_back(std::make_pair(size_t(0), size_t(0)));  // No offset info available
        }
    } else {
        // Use proper offsets from SentencePieceText
        for (int i = 0; i < spt.pieces_size(); ++i) {
            const auto &piece = spt.pieces(i);
            result.ids.push_back(static_cast<int64_t>(piece.id()));
            result.offsets.push_back(std::make_pair(static_cast<size_t>(piece.begin()),
                                                    static_cast<size_t>(piece.end())));
        }
    }

    // Add </s> (EOS) token
    result.ids.push_back(EOS_TOKEN_ID);
    result.offsets.push_back(std::make_pair(text.size(), text.size()));  // EOS has no text position

    return result;
}

#endif  // HAVE_SENTENCEPIECE

// ============================================================================
// NERModelManager Implementation
// ============================================================================

NERModelManager::NERModelManager()
    : result_cache_(std::make_unique<LRUCache<std::string, std::vector<NEREntity>>>(DEFAULT_CACHE_SIZE)) {
#if HAVE_OPENVINO
    status_.store(NERStatus::NOT_LOADED);
    status_message_ = "OpenVINO available, model not loaded";
    core_ = std::make_shared<ov::Core>();
#else
    status_.store(NERStatus::NOT_LOADED);
    status_message_ = "NER not available: OpenVINO not installed";
#endif
}

NERModelManager& NERModelManager::Instance() {
    static NERModelManager instance;
    return instance;
}

bool NERModelManager::IsAvailable() const {
#if HAVE_OPENVINO
    return status_.load() == NERStatus::LOADED;
#else
    return false;
#endif
}

void NERModelManager::SetStatusMessage(const std::string &message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_message_ = message;
}

std::string NERModelManager::GetStatusMessage() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_message_;
}

/**
 * Base directory for all NER model assets: ~/.duckdb/extensions/anofox/ner
 * Returns an empty path when no home directory can be determined.
 */
static std::filesystem::path NERModelBaseDir() {
#ifdef _WIN32
    // Windows: use USERPROFILE environment variable
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::filesystem::path(userprofile) / ".duckdb" / "extensions" / "anofox" / "ner";
    }
#else
    // Unix-like: use HOME environment variable
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".duckdb" / "extensions" / "anofox" / "ner";
    }
#endif
    return {};
}

/**
 * Per-model asset directory derived from the metadata registry:
 * <base>/<model name> (issue #56: one directory per model so tokenizer and
 * model assets always form a matched pair).
 */
static std::filesystem::path NERModelDir(const ModelMetadata &metadata) {
    auto base = NERModelBaseDir();
    if (base.empty()) {
        return {};
    }
    return base / metadata.name;
}

/**
 * Versions before issue #56 stored the DistilBERT assets directly in the
 * base directory. Move them into the per-model directory so existing
 * installations do not re-download ~66 MB.
 */
static void MigrateLegacyAssets(const ModelMetadata &metadata,
                                const std::filesystem::path &model_dir) {
    if (metadata.name != "distilbert-en" || model_dir.empty()) {
        return;
    }
    namespace fs = std::filesystem;
    try {
        const auto base = model_dir.parent_path();
        const auto legacy_model = base / metadata.model_file;
        const auto legacy_tokenizer = base / metadata.tokenizer_file;
        const auto new_model = model_dir / metadata.model_file;
        const auto new_tokenizer = model_dir / metadata.tokenizer_file;
        if (fs::exists(legacy_model) && fs::exists(legacy_tokenizer) &&
            !fs::exists(new_model) && !fs::exists(new_tokenizer)) {
            fs::create_directories(model_dir);
            fs::rename(legacy_model, new_model);
            fs::rename(legacy_tokenizer, new_tokenizer);
            AnofoxTrace(AnofoxLogLevel::Info,
                        "ner: Migrated legacy model assets to " + model_dir.string());
        }
    } catch (const std::exception &e) {
        AnofoxTrace(AnofoxLogLevel::Warn,
                    std::string("ner: Legacy asset migration failed: ") + e.what());
    }
}

/**
 * True when the model and tokenizer of a model are on disk, either in the
 * per-model directory or (DistilBERT only) in the legacy flat layout that
 * MigrateLegacyAssets moves on load.
 */
static bool ModelAssetsOnDisk(const ModelMetadata &metadata, const std::filesystem::path &model_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(model_dir / metadata.model_file, ec) && fs::exists(model_dir / metadata.tokenizer_file, ec)) {
        return true;
    }
    if (metadata.name == "distilbert-en") {
        const auto base = model_dir.parent_path();
        return fs::exists(base / metadata.model_file, ec) && fs::exists(base / metadata.tokenizer_file, ec);
    }
    return false;
}

static std::string NotDownloadedMessage(const ModelMetadata &metadata, const std::filesystem::path &model_dir) {
    return "NER model not downloaded: anofox_ner_auto_download is false. "
           "SET anofox_ner_auto_download = true to download it from Hugging Face, "
           "or place " + metadata.model_file + " and " + metadata.tokenizer_file + " in " + model_dir.string();
}

NERStatusSnapshot NERModelManager::GetStatusSnapshot() const {
    NERStatusSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot.status = status_.load();
        snapshot.message = status_message_;
        snapshot.model_path = model_path_;
        snapshot.model_name = current_model_name_;
        snapshot.device = device_name_;
    }
    if (snapshot.model_path.empty()) {
        snapshot.model_path = DefaultModelPath();
    }
#if HAVE_OPENVINO
    // Before the first detection the model is NOT_LOADED. When auto download
    // is off and the assets are not on disk, the first detection will not
    // load it either: report NOT_DOWNLOADED right away instead of after the
    // first (silently NER-less) query. The stored status is left alone so
    // placing the files on disk still lets the next detection load them.
    if (snapshot.status == NERStatus::NOT_LOADED && !auto_download_.load()) {
        const auto *metadata = GetModelMetadata(GetCurrentModelName());
        if (metadata) {
            const auto model_dir = NERModelDir(*metadata);
            if (!model_dir.empty() && !ModelAssetsOnDisk(*metadata, model_dir)) {
                snapshot.status = NERStatus::NOT_DOWNLOADED;
                snapshot.message = NotDownloadedMessage(*metadata, model_dir);
            }
        }
    }
#endif
    return snapshot;
}

std::string NERModelManager::DefaultModelPath() {
    const auto *metadata = GetModelMetadata(GetCurrentModelName());
    if (!metadata) {
        return "";
    }
    auto model_dir = NERModelDir(*metadata);
    if (model_dir.empty()) {
        return "";
    }
    return (model_dir / metadata->model_file).string();
}

std::string NERModelManager::GetModelPath() const {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!model_path_.empty()) {
            return model_path_;
        }
    }
    return DefaultModelPath();
}

double NERModelManager::GetModelSizeMB() const {
    std::string path = GetModelPath();
    if (path.empty()) return 0.0;

    try {
        if (std::filesystem::exists(path)) {
            auto size = std::filesystem::file_size(path);
            return static_cast<double>(size) / (1024.0 * 1024.0);
        }
    } catch (...) {
        // Ignore errors
    }
    return 0.0;
}

void NERModelManager::EnsureInitialized() {
#if HAVE_OPENVINO
    // Check if already initialized
    if (status_.load() == NERStatus::LOADED) {
        return;
    }

    std::lock_guard<std::mutex> lock(init_lock_);

    // Double-check after acquiring lock: another thread may have loaded the
    // model, or failed / skipped the download, while this one waited
    if (status_.load() != NERStatus::NOT_LOADED) {
        return;
    }

    LoadModel();
#else
    status_.store(NERStatus::NOT_AVAILABLE);
    SetStatusMessage("OpenVINO not compiled in");
#endif
}

void NERModelManager::LoadModel() {
#if HAVE_OPENVINO
    AnofoxTrace(AnofoxLogLevel::Info, "ner: Loading NER model...");

    // Resolve the configured model in the metadata registry; all asset paths
    // and download URLs are derived from the metadata (issue #56)
    const std::string model_name = GetCurrentModelName();
    const ModelMetadata *metadata = GetModelMetadata(model_name);
    if (!metadata) {
        status_.store(NERStatus::FAILED);
        SetStatusMessage("Unknown NER model '" + model_name + "'. Valid models: " + GetValidModelNames());
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Unknown model: " + model_name);
        return;
    }

    const std::filesystem::path model_dir = NERModelDir(*metadata);
    if (model_dir.empty()) {
        status_.store(NERStatus::FAILED);
        SetStatusMessage("Cannot determine NER model directory (HOME/USERPROFILE not set)");
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Cannot determine model directory");
        return;
    }
    const std::string model_path = (model_dir / metadata->model_file).string();
    const std::string tokenizer_path = (model_dir / metadata->tokenizer_file).string();
    const std::string device_name = GetDevice();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        model_path_ = model_path;
        current_model_name_ = model_name;
    }

    // Reuse assets downloaded by older versions into the flat base directory
    MigrateLegacyAssets(*metadata, model_dir);

    // Model and tokenizer must be present as a matched pair; if either is
    // missing, (re-)download both from the metadata URLs
    if (!std::filesystem::exists(model_path) || !std::filesystem::exists(tokenizer_path)) {
        if (!auto_download_.load()) {
            status_.store(NERStatus::NOT_DOWNLOADED);
            SetStatusMessage(NotDownloadedMessage(*metadata, model_dir));
            AnofoxTrace(AnofoxLogLevel::Info,
                        "ner: Model assets not found and auto download is off; NER detection is skipped");
            return;
        }
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Model assets not found, attempting download...");
        status_.store(NERStatus::DOWNLOADING);
        SetStatusMessage("Downloading model from HuggingFace...");

        // Create directory if it doesn't exist
        std::filesystem::create_directories(model_dir);

        if (!DownloadModel(*metadata, model_path, tokenizer_path)) {
            status_.store(NERStatus::FAILED);
            SetStatusMessage("Failed to download model");
            AnofoxTrace(AnofoxLogLevel::Error, "ner: Model download failed");
            return;
        }
    }

    try {
        // Read model (supports .onnx directly via ONNX frontend)
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Reading ONNX model with OpenVINO...");
        std::shared_ptr<ov::Model> model = core_->read_model(model_path);

        // Reshape model to accept dynamic batch and sequence length
        // The ONNX model has dynamic shapes, but OpenVINO needs explicit configuration
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Configuring dynamic input shapes...");
        // BERT exports also take token_type_ids, DistilBERT exports do not;
        // the inputs are read from the model instead of the registry
        ov::PartialShape dynamic_shape = {1, ov::Dimension::dynamic()};  // batch=1, seq=dynamic
        std::map<std::string, ov::PartialShape> input_shapes;
        bool has_input_ids = false;
        bool has_token_type_ids = false;
        for (const auto &input : model->inputs()) {
            const auto &names = input.get_names();
            const std::string input_name = names.empty() ? std::string() : *names.begin();
            if (names.count("input_ids")) {
                has_input_ids = true;
                input_shapes["input_ids"] = dynamic_shape;
            } else if (names.count("attention_mask")) {
                input_shapes["attention_mask"] = dynamic_shape;
            } else if (names.count("token_type_ids")) {
                has_token_type_ids = true;
                input_shapes["token_type_ids"] = dynamic_shape;
            } else {
                throw std::runtime_error("NER model input '" + input_name +
                                         "' is not supported (expected input_ids, attention_mask, token_type_ids)");
            }
        }
        if (!has_input_ids || !input_shapes.count("attention_mask")) {
            throw std::runtime_error("NER model must take input_ids and attention_mask");
        }
        AnofoxTrace(AnofoxLogLevel::Debug, std::string("ner: Model inputs: input_ids, attention_mask") +
                                               (has_token_type_ids ? ", token_type_ids" : ""));
        model->reshape(input_shapes);

        // Log available devices for diagnostics
        auto available = core_->get_available_devices();
        std::string dev_list;
        for (const auto &d : available) dev_list += d + " ";
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Available OpenVINO devices: " + dev_list);

        // Create and load the tokenizer matching the model's metadata. A
        // model without a working tokenizer must not report LOADED, otherwise
        // inference silently returns no entities (issue #56)
        if (metadata->tokenizer_type == "wordpiece") {
            // BERT and DistilBERT use a WordPiece tokenizer (tokenizer.json)
            tokenizer_ = std::make_unique<WordPieceTokenizer>();
        }
#if HAVE_SENTENCEPIECE
        else if (metadata->tokenizer_type == "sentencepiece") {
            // XLM-RoBERTa class models use SentencePiece tokenizers
            tokenizer_ = std::make_unique<SentencePieceTokenizer>();
        }
#endif
        else {
            status_.store(NERStatus::FAILED);
            SetStatusMessage("Tokenizer type '" + metadata->tokenizer_type +
                             "' is not supported in this build");
            AnofoxTrace(AnofoxLogLevel::Error,
                        "ner: Unsupported tokenizer type: " + metadata->tokenizer_type);
            return;
        }

        if (!tokenizer_->Load(tokenizer_path)) {
            status_.store(NERStatus::FAILED);
            SetStatusMessage("Failed to load tokenizer from " + tokenizer_path);
            AnofoxTrace(AnofoxLogLevel::Error,
                        "ner: Failed to load tokenizer: " + tokenizer_path);
            return;
        }

        // Bound inputs at the model's maximum sequence length (issue #56);
        // these are written before LOADED is published, immutable afterwards
        max_seq_length_ = metadata->max_seq_length;
        labels_ = metadata->labels;
        use_token_type_ids_ = has_token_type_ids;
        fixed_seq_length_ = 0;

        // Compile model for configured device (default AUTO: selects GPU if available, falls back to CPU)
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Compiling model for device: " + device_name);
        compiled_model_ = std::make_shared<ov::CompiledModel>(
            core_->compile_model(model, device_name)
        );

        // Some exports fail at inference time with a dynamic sequence
        // length (the OpenVINO CPU plugin rejects the Xenova DistilBERT
        // multilingual export with "bad mem size" in the first attention
        // MatMul), while the same graph runs with a fixed length. Probe once
        // and fall back to a fixed length instead of reporting LOADED for a
        // model whose every inference fails.
        std::string probe_error = ProbeInference();
        if (!probe_error.empty()) {
            const size_t fixed_length = std::min(NER_WINDOW_TOKENS, max_seq_length_);
            AnofoxTrace(AnofoxLogLevel::Warn,
                        "ner: inference with a dynamic sequence length failed for " + model_name + " (" +
                            probe_error + "); compiling the model for a fixed length of " +
                            std::to_string(fixed_length) + " tokens, which is slower for short texts");
            for (auto &entry : input_shapes) {
                entry.second = ov::PartialShape {1, static_cast<int64_t>(fixed_length)};
            }
            model->reshape(input_shapes);
            compiled_model_ = std::make_shared<ov::CompiledModel>(core_->compile_model(model, device_name));
            fixed_seq_length_ = fixed_length;
            probe_error = ProbeInference();
            if (!probe_error.empty()) {
                throw std::runtime_error("NER model inference failed: " + probe_error);
            }
        }

        // Log the actual execution device(s) chosen by AUTO or by explicit selection
        auto exec_devices = compiled_model_->get_property(ov::execution_devices);
        std::string exec_dev_str;
        for (size_t di = 0; di < exec_devices.size(); ++di) {
            if (di > 0) exec_dev_str += ",";
            exec_dev_str += exec_devices[di];
        }
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Executing on: " + exec_dev_str);
        if (fixed_seq_length_ > 0) {
            exec_dev_str += ", fixed length " + std::to_string(fixed_seq_length_);
        }

        // Note: inference requests are created per ExtractEntities call;
        // a single ov::InferRequest is stateful and must not be shared
        // across DuckDB worker threads (issue #50)

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            status_message_ = "Model loaded successfully (" + model_name + ", device: " + exec_dev_str + ")";
        }
        // Publish LOADED last: readers that observe LOADED are guaranteed to
        // see the fully constructed compiled_model_ and tokenizer_
        status_.store(NERStatus::LOADED);
        AnofoxTrace(AnofoxLogLevel::Info, "ner: NER model loaded from: " + model_path);

    } catch (const ov::Exception &e) {
        status_.store(NERStatus::FAILED);
        SetStatusMessage(std::string("OpenVINO error: ") + e.what());
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Failed to load model: " + std::string(e.what()));
    } catch (const std::exception &e) {
        status_.store(NERStatus::FAILED);
        SetStatusMessage(std::string("Error: ") + e.what());
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Failed to load model: " + std::string(e.what()));
    }
#else
    status_.store(NERStatus::NOT_AVAILABLE);
    SetStatusMessage("OpenVINO not compiled in");
#endif
}

namespace {

// RAII wrappers for the C resources used during download (issue #56)
struct FileDeleter {
    void operator()(std::FILE *file) const noexcept {
        if (file) {
            fclose(file);
        }
    }
};
using FileHandle = std::unique_ptr<std::FILE, FileDeleter>;

struct CurlDeleter {
    void operator()(CURL *curl) const noexcept {
        if (curl) {
            curl_easy_cleanup(curl);
        }
    }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

/**
 * Download `url` into a `<dest>.tmp` file and atomically rename it to `dest`
 * on success, so a partially written file can never be mistaken for a valid
 * asset. Returns an empty string on success, an error description otherwise.
 */
std::string DownloadToFileAtomic(const std::string &url, const std::string &dest) {
    const std::string tmp_path = dest + ".tmp";

    {
        FileHandle file(fopen(tmp_path.c_str(), "wb"));
        if (!file) {
            return "failed to open file for writing: " + tmp_path;
        }

        CurlHandle curl(curl_easy_init());
        if (!curl) {
            return "failed to initialize curl";
        }

        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteDataToFile);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, file.get());
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 600L);  // 10 minute timeout for large model
        curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "anofox-tabular/1.0");
        ConfigureCurlCaBundle(curl.get(), "ner");

        CURLcode res = curl_easy_perform(curl.get());
        long http_code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

        if (res != CURLE_OK) {
            std::error_code ignored;
            std::filesystem::remove(tmp_path, ignored);
            return std::string(curl_easy_strerror(res)) + " (HTTP " + std::to_string(http_code) + ")" +
                   CurlCaErrorHint(res);
        }
        // FileHandle goes out of scope here, flushing and closing the file
        // before the rename below
    }

    try {
        std::filesystem::rename(tmp_path, dest);
    } catch (const std::exception &e) {
        std::error_code ignored;
        std::filesystem::remove(tmp_path, ignored);
        return std::string("failed to rename downloaded file: ") + e.what();
    }
    return "";
}

} // anonymous namespace

bool NERModelManager::DownloadModel(const ModelMetadata &metadata, const std::string &model_dest,
                                    const std::string &tokenizer_dest) {
    // Download both assets of the model described by the metadata registry;
    // URLs are never hard-coded here (issue #56)
    struct FileToDownload {
        std::string url;
        std::string dest;
        std::string name;
    };

    const std::vector<FileToDownload> files = {
        {metadata.tokenizer_url, tokenizer_dest, metadata.tokenizer_file},
        {metadata.model_url, model_dest, metadata.model_file}
    };

    const int max_retries = 3;

    for (const auto &file : files) {
        bool download_success = false;

        for (int attempt = 1; attempt <= max_retries && !download_success; attempt++) {
            if (attempt > 1) {
                AnofoxTrace(AnofoxLogLevel::Info,
                    "ner: Retrying download attempt " + std::to_string(attempt) + "/" + std::to_string(max_retries));
            } else {
                AnofoxTrace(AnofoxLogLevel::Info, "ner: Downloading " + file.name + " from HuggingFace...");
            }

            SetStatusMessage("Downloading " + file.name + "...");

            std::string error = DownloadToFileAtomic(file.url, file.dest);
            if (error.empty()) {
                download_success = true;
                AnofoxTrace(AnofoxLogLevel::Info, "ner: Download complete: " + file.name);
            } else {
                AnofoxTrace(AnofoxLogLevel::Warn,
                    "ner: Download attempt " + std::to_string(attempt) + " failed: " + error);

                if (attempt < max_retries) {
                    int wait_time = 1 << attempt;
                    AnofoxTrace(AnofoxLogLevel::Info,
                        "ner: Waiting " + std::to_string(wait_time) + "s before retry");
                    std::this_thread::sleep_for(std::chrono::seconds(wait_time));
                }
            }
        }

        if (!download_success) {
            AnofoxTrace(AnofoxLogLevel::Error,
                "ner: Failed to download " + file.name + " after " + std::to_string(max_retries) + " attempts");
            SetStatusMessage("Failed to download " + file.name);
            return false;
        }
    }

    // Both assets must exist as a matched pair before declaring success
    if (!std::filesystem::exists(model_dest) || !std::filesystem::exists(tokenizer_dest)) {
        AnofoxTrace(AnofoxLogLevel::Error,
            "ner: Download finished but assets are incomplete (model/tokenizer pair mismatch)");
        SetStatusMessage("Download incomplete: model/tokenizer asset pair mismatch");
        return false;
    }

    SetStatusMessage("Download complete");
    return true;
}

std::string NERModelManager::ProbeInference() {
#if HAVE_OPENVINO
    // A short input (padded to the minimum length) and a longer one, so both
    // the padded and the unpadded path run once before LOADED is published
    static const std::vector<std::string> probes = {
        "Sabine Weiß lives in Berlin.",
        "Sabine Weiß from Allianz called about the claim of Jürgen Schäfer in München; please call her back "
        "before Friday, the documents are complete."};
    try {
        for (const auto &probe : probes) {
            InferSequence(tokenizer_->Encode(probe), probe);
        }
    } catch (const std::exception &e) {
        std::string message = e.what();
        // OpenVINO messages span several lines; keep the trace line on one
        std::replace(message.begin(), message.end(), '\n', ' ');
        return message;
    }
#endif
    return "";
}

namespace {
// The client context of the innermost NERInterruptScope of this thread (nullptr: none)
thread_local ClientContext *ner_interrupt_context = nullptr;
} // namespace

NERInterruptScope::NERInterruptScope(ClientContext *context) : previous_(ner_interrupt_context) {
    ner_interrupt_context = context;
}

NERInterruptScope::~NERInterruptScope() {
    ner_interrupt_context = previous_;
}

void NERInterruptScope::Check() {
    if (ner_interrupt_context && ner_interrupt_context->IsInterrupted()) {
        AnofoxTrace(AnofoxLogLevel::Debug, "ner: query interrupted, stopping inference");
        throw InterruptException();
    }
}

std::vector<NEREntity> NERModelManager::RunSingleInference(const std::string &text) {
#if HAVE_OPENVINO
    // Check cache first. Get/Put are fully synchronized internally and Put is
    // a safe no-op when the capacity is 0, so no unsynchronized
    // Capacity() pre-check is needed (issue #50)
    if (result_cache_) {
        auto cached = result_cache_->Get(text);
        if (cached) {
            AnofoxTrace(AnofoxLogLevel::Debug, "ner: Cache hit");
            return *cached;
        }
    }

    // Before the (slow) inference, outside the try below, which turns errors into "no entities"
    NERInterruptScope::Check();

    try {
        // tokenizer_ and compiled_model_ are immutable once status_ is
        // LOADED (checked by the callers), so reading them here is safe
        if (!tokenizer_ || !tokenizer_->IsLoaded()) {
            AnofoxTrace(AnofoxLogLevel::Error, "ner: Tokenizer not loaded");
            return {};
        }

        // Tokenize input; ids and offsets are returned by value so no
        // tokenizer state is shared between concurrent calls (issue #50)
        TokenizedInput tokenized = tokenizer_->Encode(text);
        if (tokenized.ids.size() < 2) {
            return {};
        }

        // Longer inputs run as overlapping windows: the model accepts at
        // most max_seq_length_ tokens (issue #56) and misses entities at late
        // positions well before that (see NER_WINDOW_TOKENS)
        const size_t window_tokens = std::min(NER_WINDOW_TOKENS, max_seq_length_);
        std::vector<NEREntity> entities;
        if (tokenized.ids.size() <= window_tokens) {
            entities = InferSequence(std::move(tokenized), text);
        } else {
            // The windows keep the original byte offsets, so their entities
            // map straight back to the text; each window reports the
            // entities that start in its own part of the overlap
            const size_t content_tokens = tokenized.ids.size() - 2;
            const auto windows = PlanNERWindows(content_tokens, window_tokens - 2, NER_WINDOW_OVERLAP);
            AnofoxTrace(AnofoxLogLevel::Debug,
                        "ner: input of " + std::to_string(tokenized.ids.size()) + " tokens split into " +
                            std::to_string(windows.size()) + " windows of up to " +
                            std::to_string(window_tokens) + " tokens");
            std::vector<NEREntity> collected;
            for (const auto &window : windows) {
                const size_t own_begin = tokenized.offsets[window.own_begin + 1].first;
                const size_t own_end = window.own_end >= content_tokens
                                           ? std::string::npos
                                           : tokenized.offsets[window.own_end + 1].first;
                for (auto &entity : InferSequence(SliceTokenizedWindow(tokenized, window), text)) {
                    if (entity.start_pos >= own_begin && entity.start_pos < own_end) {
                        collected.push_back(std::move(entity));
                    }
                }
            }
            entities = CombineWindowEntities(std::move(collected), text);
        }

        // Store in cache (no-op when the cache is disabled)
        if (result_cache_) {
            result_cache_->Put(text, entities);
        }

        return entities;

    } catch (const ov::Exception &e) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: OpenVINO inference error: " + std::string(e.what()));
        return {};
    } catch (const std::exception &e) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Inference error: " + std::string(e.what()));
        return {};
    }
#else
    return {};
#endif
}

std::vector<NEREntity> NERModelManager::InferSequence(TokenizedInput tokenized, const std::string &text) {
#if HAVE_OPENVINO
    size_t actual_length = tokenized.ids.size();

    // Pad to minimum length to avoid OpenVINO shape inference issues
    // with very short sequences in the quantized model
    static const size_t MIN_SEQ_LENGTH = 16;
    size_t seq_length = std::max(actual_length, MIN_SEQ_LENGTH);
    if (fixed_seq_length_ > 0) {
        // The model was compiled for one sequence length (see LoadModel);
        // windows never exceed it
        if (actual_length > fixed_seq_length_) {
            throw std::runtime_error("NER input of " + std::to_string(actual_length) +
                                     " tokens exceeds the fixed model length " + std::to_string(fixed_seq_length_));
        }
        seq_length = fixed_seq_length_;
    }

    // Create attention mask (1 for real tokens, 0 for padding)
    std::vector<int64_t> attention_mask(seq_length, 0);
    for (size_t i = 0; i < actual_length; ++i) {
        attention_mask[i] = 1;
    }

    // Pad input_ids with 0 (PAD token)
    tokenized.ids.resize(seq_length, 0);

    // Create input tensors
    ov::Shape input_shape = {1, seq_length};

    ov::Tensor input_ids_tensor(ov::element::i64, input_shape);
    ov::Tensor attention_mask_tensor(ov::element::i64, input_shape);
    const size_t num_labels = labels_.size();

    // Copy data to tensors
    int64_t* input_ids_data = input_ids_tensor.data<int64_t>();
    int64_t* attention_data = attention_mask_tensor.data<int64_t>();
    std::memcpy(input_ids_data, tokenized.ids.data(), tokenized.ids.size() * sizeof(int64_t));
    std::memcpy(attention_data, attention_mask.data(), attention_mask.size() * sizeof(int64_t));

    // Create a per-call inference request: ov::CompiledModel is
    // thread-safe for creating requests, while a single ov::InferRequest
    // is stateful and must not be shared across threads (issue #50)
    ov::InferRequest infer_request = compiled_model_->create_infer_request();

    // Set input tensors
    infer_request.set_tensor("input_ids", input_ids_tensor);
    infer_request.set_tensor("attention_mask", attention_mask_tensor);
    if (use_token_type_ids_) {
        // A single segment: every token belongs to sentence A (type 0)
        ov::Tensor token_type_ids_tensor(ov::element::i64, input_shape);
        std::fill_n(token_type_ids_tensor.data<int64_t>(), seq_length, int64_t(0));
        infer_request.set_tensor("token_type_ids", token_type_ids_tensor);
    }

    // Run inference
    infer_request.infer();

    // Get output tensor and validate its shape before reading any logits;
    // a mismatch would mean an out-of-bounds read (issue #56)
    ov::Tensor output_tensor = infer_request.get_output_tensor(0);
    const ov::Shape &output_shape = output_tensor.get_shape();
    ValidateLogitsShape(output_shape, seq_length, num_labels);
    const float* logits_data = output_tensor.data<float>();

    std::vector<float> logits(logits_data, logits_data + seq_length * num_labels);

    return PostProcess(logits, seq_length, text, tokenized.offsets);
#else
    return {};
#endif
}

std::vector<NEREntity> NERModelManager::ExtractEntities(const std::string &text) {
#if HAVE_OPENVINO
    // A value without any word (hex digests, ids, numbers) holds nothing the
    // model could tag; skipping it saves a full inference per value
    if (!IsAvailable() || !HasNaturalLanguageWord(text)) {
        return {};
    }
    return RunSingleInference(text);
#else
    return {};
#endif
}

std::vector<NEREntity> NERModelManager::PostProcess(
    const std::vector<float> &logits,
    size_t seq_length,
    const std::string &text,
    const std::vector<std::pair<size_t, size_t>> &offsets
) {
    std::vector<NEREntity> entities;
    const auto &labels = labels_;
    const size_t num_labels = labels.size();

    // Skip [CLS] and [SEP] tokens
    size_t start_idx = 1;
    size_t end_idx = seq_length - 1;

    std::string current_entity;
    std::string current_label;
    size_t entity_start = 0;
    size_t entity_end = 0;
    double entity_confidence = 0.0;
    int entity_token_count = 0;

    for (size_t i = start_idx; i < end_idx && i < offsets.size(); ++i) {
        // Get logits for this position
        const float* pos_logits = &logits[i * num_labels];

        // Apply softmax and find max
        float max_logit = *std::max_element(pos_logits, pos_logits + num_labels);
        float sum_exp = 0.0f;
        for (size_t j = 0; j < num_labels; ++j) {
            sum_exp += std::exp(pos_logits[j] - max_logit);
        }

        size_t best_label_idx = 0;
        float best_prob = 0.0f;
        for (size_t j = 0; j < num_labels; ++j) {
            float prob = std::exp(pos_logits[j] - max_logit) / sum_exp;
            if (prob > best_prob) {
                best_prob = prob;
                best_label_idx = j;
            }
        }

        const std::string &label = labels[best_label_idx];

        // Check if this is a B- (begin) or I- (inside) tag; a tag of a type
        // that is not reported (DATE) is read as O
        bool is_begin = label.size() > 2 && label[0] == 'B' && label[1] == '-';
        bool is_inside = label.size() > 2 && label[0] == 'I' && label[1] == '-';
        std::string entity_type = (is_begin || is_inside) ? label.substr(2) : "";
        if (!entity_type.empty() && !IsReportedNEREntityType(entity_type)) {
            is_begin = false;
            is_inside = false;
            entity_type.clear();
        }

        if (is_begin) {
            // Save previous entity if exists
            if (!current_entity.empty() && entity_token_count > 0) {
                entities.emplace_back(
                    current_entity,
                    current_label,
                    entity_start,
                    entity_end,
                    entity_confidence / entity_token_count
                );
            }

            // Start new entity
            current_entity = text.substr(offsets[i].first, offsets[i].second - offsets[i].first);
            current_label = entity_type;
            entity_start = offsets[i].first;
            entity_end = offsets[i].second;
            entity_confidence = best_prob;
            entity_token_count = 1;

        } else if (is_inside && entity_type == current_label) {
            // Continue current entity
            size_t token_start = offsets[i].first;
            size_t token_end = offsets[i].second;

            // Add space if there's a gap
            if (token_start > entity_end) {
                current_entity += text.substr(entity_end, token_start - entity_end);
            }
            current_entity += text.substr(token_start, token_end - token_start);
            entity_end = token_end;
            entity_confidence += best_prob;
            entity_token_count++;

        } else {
            // End current entity
            if (!current_entity.empty() && entity_token_count > 0) {
                entities.emplace_back(
                    current_entity,
                    current_label,
                    entity_start,
                    entity_end,
                    entity_confidence / entity_token_count
                );
            }
            current_entity.clear();
            current_label.clear();
            entity_token_count = 0;
        }
    }

    // Save final entity if exists
    if (!current_entity.empty() && entity_token_count > 0) {
        entities.emplace_back(
            current_entity,
            current_label,
            entity_start,
            entity_end,
            entity_confidence / entity_token_count
        );
    }

    // Merge consecutive entities of same type to fix model fragmentation
    // Example: ["Don", "au", "wo", "ert", "h"] all tagged as LOCATION
    // -> merge to single entity "Donauwoerth"
    return MergeConsecutiveSameTypeEntities(entities);
}

// ============================================================================
// Cache Management Methods
// ============================================================================

size_t NERModelManager::GetCacheSize() const {
    if (result_cache_) {
        return result_cache_->Size();
    }
    return 0;
}

size_t NERModelManager::GetCacheCapacity() const {
    if (result_cache_) {
        return result_cache_->Capacity();
    }
    return 0;
}

void NERModelManager::SetCacheCapacity(size_t capacity) {
    if (result_cache_) {
        result_cache_->SetCapacity(capacity);
        AnofoxTrace(AnofoxLogLevel::Info,
            "ner: Cache capacity set to " + std::to_string(capacity));
    }
}

void NERModelManager::ClearCache() {
    if (result_cache_) {
        result_cache_->Clear();
        AnofoxTrace(AnofoxLogLevel::Debug, "ner: Cache cleared");
    }
}

void NERModelManager::SetDevice(const std::string &device_name) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        device_name_ = device_name;
    }
    if (status_.load() == NERStatus::LOADED) {
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Device set to '" + device_name +
                                              "'; the model is already loaded, so it takes effect in a new process");
        return;
    }
    AnofoxTrace(AnofoxLogLevel::Info, "ner: Device set to '" + device_name + "' (takes effect at the next model load)");
    RequestReload("anofox_ner_device changed");
}

namespace {

// An OpenVINO device without a virtual-device prefix: CPU, or GPU / NPU with
// an optional ".<index>" (GPU.0, NPU.1)
bool IsPhysicalDeviceName(const std::string &name) {
    if (name == "CPU" || name == "GPU" || name == "NPU") {
        return true;
    }
    if ((name.rfind("GPU.", 0) == 0 || name.rfind("NPU.", 0) == 0) && name.size() > 4) {
        return std::all_of(name.begin() + 4, name.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }
    return false;
}

const char *NERStatusName(NERStatus status) {
    switch (status) {
        case NERStatus::NOT_LOADED: return "NOT_LOADED";
        case NERStatus::DOWNLOADING: return "DOWNLOADING";
        case NERStatus::LOADED: return "LOADED";
        case NERStatus::FAILED: return "FAILED";
        case NERStatus::NOT_AVAILABLE: return "NOT_AVAILABLE";
        case NERStatus::NOT_DOWNLOADED: return "NOT_DOWNLOADED";
    }
    return "UNKNOWN";
}

} // anonymous namespace

std::string NERModelManager::NormalizeDeviceName(const std::string &device_name) {
    // OpenVINO spells devices in upper case; spaces around list items are dropped
    std::string normalized;
    for (unsigned char c : device_name) {
        if (!std::isspace(c)) {
            normalized += static_cast<char>(std::toupper(c));
        }
    }

    bool valid = normalized == "AUTO" || IsPhysicalDeviceName(normalized);
    const auto colon = normalized.find(':');
    if (!valid && colon != std::string::npos) {
        // Virtual devices over a list of physical devices: AUTO:GPU,CPU,
        // HETERO:GPU,CPU, MULTI:GPU.1,CPU
        const std::string prefix = normalized.substr(0, colon);
        if (prefix == "AUTO" || prefix == "HETERO" || prefix == "MULTI") {
            const std::string list = normalized.substr(colon + 1);
            valid = !list.empty();
            size_t start = 0;
            while (valid && start <= list.size()) {
                const size_t comma = (std::min)(list.find(',', start), list.size());
                valid = IsPhysicalDeviceName(list.substr(start, comma - start));
                start = comma + 1;
            }
        }
    }
    if (!valid) {
        throw InvalidInputException("Unsupported anofox_ner_device '" + device_name +
                                    "'. Use AUTO, CPU, GPU, NPU, an indexed device such as GPU.0, or AUTO:, HETERO: "
                                    "or MULTI: followed by a comma-separated device list (HETERO:GPU,CPU)");
    }
    return normalized;
}

bool NERModelManager::RequestReload(const std::string &reason) {
    // Only a load that ended without a model is retried; LOADED (immutable
    // model) and DOWNLOADING (a load in progress) are left alone
    for (auto from : {NERStatus::FAILED, NERStatus::NOT_DOWNLOADED}) {
        auto expected = from;
        if (status_.compare_exchange_strong(expected, NERStatus::NOT_LOADED)) {
            SetStatusMessage(std::string("Model not loaded yet: ") + reason + " after status " + NERStatusName(from) +
                             "; the next detection loads it again");
            AnofoxTrace(AnofoxLogLevel::Info, std::string("ner: ") + reason + ", status " + NERStatusName(from) +
                                                  " cleared; the next detection loads the model again");
            return true;
        }
    }
    return false;
}

void NERModelManager::RequireModel() {
    if (status_.load() == NERStatus::LOADED) {
        return;
    }
    // Loads the model, or waits for a load in progress in another thread
    EnsureInitialized();
    const auto snapshot = GetStatusSnapshot();
    std::string hint;
    switch (snapshot.status) {
        case NERStatus::NOT_DOWNLOADED:
            // The status message already says how to get the model
            break;
        case NERStatus::FAILED:
            hint = " Fix the cause shown by anofox_ner_status(): SET anofox_ner_device or anofox_ner_model makes the "
                   "next detection load the model again (e.g. SET anofox_ner_device = 'CPU').";
            break;
        case NERStatus::NOT_AVAILABLE:
            hint = " This build has no NER support (OpenVINO is not included).";
            break;
        default:
            return;  // LOADED
    }
    std::string message = snapshot.message;
    while (!message.empty() && std::isspace(static_cast<unsigned char>(message.back()))) {
        message.pop_back();
    }
    AnofoxTrace(AnofoxLogLevel::Warn, std::string("ner: anofox_ner_required is true but the model is ") +
                                          NERStatusName(snapshot.status));
    throw InvalidInputException(std::string("anofox_ner_required is true but the NER model is not available (status ") +
                                NERStatusName(snapshot.status) + ": " + message + ")." + hint +
                                " Or SET anofox_ner_required = false to fall back to the name dictionary for NAME "
                                "(ORGANIZATION, LOCATION and MISC are then not detected), or restrict "
                                "anofox_pii_enabled_types to types that need no NER model.");
}

void NERModelManager::SetAutoDownload(bool enabled) {
    auto_download_.store(enabled);
    if (enabled) {
        // Let the next NER call retry the download it skipped
        auto expected = NERStatus::NOT_DOWNLOADED;
        status_.compare_exchange_strong(expected, NERStatus::NOT_LOADED);
    }
    AnofoxTrace(AnofoxLogLevel::Info,
                std::string("ner: Auto download ") + (enabled ? "enabled" : "disabled"));
}

std::string NERModelManager::GetDevice() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return device_name_;
}

std::vector<std::string> NERModelManager::GetAvailableDevices() const {
#if HAVE_OPENVINO
    return core_->get_available_devices();
#else
    return {};
#endif
}

// ============================================================================
// Batch Processing
// ============================================================================

std::vector<std::vector<NEREntity>> NERModelManager::ExtractEntitiesBatch(
    const std::vector<std::string> &texts
) {
    std::vector<std::vector<NEREntity>> all_results(texts.size());

#if HAVE_OPENVINO
    if (!IsAvailable() || texts.empty()) {
        return all_results;
    }

    // Deduplicate inputs; RunSingleInference consults the result cache
    // itself, so duplicate texts (and previously cached texts) only run
    // inference once
    std::unordered_map<std::string, std::vector<size_t>> text_to_indices;
    for (size_t i = 0; i < texts.size(); ++i) {
        // Texts without any word (empty values, hex digests, ids, numbers)
        // keep their default empty result, as in ExtractEntities
        if (HasNaturalLanguageWord(texts[i])) {
            text_to_indices[texts[i]].push_back(i);
        }
    }

    AnofoxTrace(AnofoxLogLevel::Debug,
                "ner: Batch processing " + std::to_string(text_to_indices.size()) +
                " unique texts (" + std::to_string(texts.size()) + " total)");

    for (const auto &entry : text_to_indices) {
        auto entities = RunSingleInference(entry.first);
        for (size_t i : entry.second) {
            all_results[i] = entities;
        }
    }
#endif

    return all_results;
}

// ============================================================================
// Configuration Options
// ============================================================================

namespace {

void SetNERCacheSizeOption(ClientContext &context, SetScope scope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_ner_cache_size cannot be NULL");
    }
    auto value = BigIntValue::Get(parameter);
    if (value < 0) {
        throw InvalidInputException("anofox_ner_cache_size cannot be negative");
    }
    NERModelManager::Instance().SetCacheCapacity(static_cast<size_t>(value));
}

void SetNERModelOption(ClientContext &context, SetScope scope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_ner_model cannot be NULL");
    }
    auto model_name = StringValue::Get(parameter);

    // Validate model name against the metadata registry: only models whose
    // assets are actually downloadable are listed (issue #56)
    if (!GetModelMetadata(model_name)) {
        throw InvalidInputException(
            "Unsupported NER model: " + model_name + ". Valid models: " + GetValidModelNames());
    }

    // Switching models after the model has been loaded (or while a load is in
    // progress) is rejected instead of silently doing nothing: the #50
    // architecture treats compiled model and tokenizer as immutable once
    // LOADED is published, so a hot swap is not safe (issue #56)
    auto snapshot = NERModelManager::Instance().GetStatusSnapshot();
    const bool load_active = snapshot.status == NERStatus::LOADED ||
                             snapshot.status == NERStatus::DOWNLOADING;
    if (load_active && model_name != snapshot.model_name) {
        throw InvalidInputException(
            "anofox_ner_model cannot be changed after the NER model has been loaded "
            "(currently '" + snapshot.model_name + "'). Restart the process to switch models.");
    }

    // Store the new model name; it takes effect on the first model load
    SetCurrentModelName(model_name);

    AnofoxTrace(AnofoxLogLevel::Info,
                "ner: Model set to '" + model_name + "' (takes effect on first model load)");

    // A load that failed (or found no assets) with the previous setting is
    // retried by the next detection
    NERModelManager::Instance().RequestReload("anofox_ner_model set");
}

void SetNERAutoDownloadOption(ClientContext &context, SetScope scope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_ner_auto_download cannot be NULL");
    }
    NERModelManager::Instance().SetAutoDownload(BooleanValue::Get(parameter));
}

void SetNERDeviceOption(ClientContext &context, SetScope scope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_ner_device cannot be NULL");
    }
    auto device_name = NERModelManager::NormalizeDeviceName(StringValue::Get(parameter));
    NERModelManager::Instance().SetDevice(device_name);
    parameter = Value(device_name);
}

void SetNERRequiredOption(ClientContext &context, SetScope scope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_ner_required cannot be NULL");
    }
    const bool required = BooleanValue::Get(parameter);
    NERModelManager::Instance().SetRequired(required);
    AnofoxTrace(AnofoxLogLevel::Info, std::string("ner: Model ") + (required ? "required" : "optional") +
                                          " for NER-based PII detection");
}

} // anonymous namespace

void RegisterNEROptions(ExtensionLoader &loader) {
    auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

    config.AddExtensionOption("anofox_ner_cache_size",
                              "LRU cache size for NER results (0 to disable)",
                              LogicalTypeId::BIGINT,
                              Value::BIGINT(static_cast<int64_t>(NERModelManager::DEFAULT_CACHE_SIZE)),
                              SetNERCacheSizeOption);

    config.AddExtensionOption("anofox_ner_model",
                              "NER model to use: " + DescribeSupportedModels() +
                                  "; must be set before the model is first loaded",
                              LogicalTypeId::VARCHAR,
                              Value(NER_DEFAULT_MODEL),
                              SetNERModelOption);

    config.AddExtensionOption("anofox_ner_auto_download",
                              "Download the NER model (66-178 MB, see anofox_ner_model) from Hugging Face on first use when it is not "
                              "on disk; false skips NER-based detection instead (offline use: NAME falls back to "
                              "a name dictionary, ORGANIZATION, LOCATION and MISC are not detected)",
                              LogicalTypeId::BOOLEAN,
                              Value::BOOLEAN(true),
                              SetNERAutoDownloadOption);

    config.AddExtensionOption("anofox_ner_device",
                              "OpenVINO device for NER inference: AUTO (default), CPU, GPU, NPU, an indexed device "
                              "(GPU.0) or AUTO:/HETERO:/MULTI: with a device list (HETERO:GPU,CPU); takes effect at "
                              "the next model load, and setting it retries a load that failed",
                              LogicalTypeId::VARCHAR,
                              Value("AUTO"),
                              SetNERDeviceOption);

    config.AddExtensionOption("anofox_ner_required",
                              "Raise an error when PII detection needs the NER model (NAME, ORGANIZATION, LOCATION, "
                              "MISC) and it is not available (NOT_DOWNLOADED, FAILED, NOT_AVAILABLE) instead of "
                              "falling back to the name dictionary",
                              LogicalTypeId::BOOLEAN,
                              Value::BOOLEAN(false),
                              SetNERRequiredOption);
}

} // namespace anofox
} // namespace duckdb
