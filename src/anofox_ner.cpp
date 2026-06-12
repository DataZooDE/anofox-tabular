#include "anofox_ner.hpp"
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
#include <cstring>
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
// Label definitions for DistilBERT NER (CoNLL 2003)
// ============================================================================

const std::vector<std::string>& NERModelManager::GetLabels() {
    // Label order matches the DistilBERT NER model (dslim/distilbert-NER)
    // The model uses: O, B-PER, I-PER, B-ORG, I-ORG, B-LOC, I-LOC, B-MISC, I-MISC
    static std::vector<std::string> labels = {
        "O",      // 0: Outside (not an entity)
        "B-PER",  // 1: Begin Person
        "I-PER",  // 2: Inside Person
        "B-ORG",  // 3: Begin Organization
        "I-ORG",  // 4: Inside Organization
        "B-LOC",  // 5: Begin Location
        "I-LOC",  // 6: Inside Location
        "B-MISC", // 7: Begin Miscellaneous
        "I-MISC"  // 8: Inside Miscellaneous
    };
    return labels;
}

// ============================================================================
// Model Metadata Registry
// ============================================================================

/**
 * Metadata for supported NER models
 */
struct ModelMetadata {
    std::string name;           // Short identifier (e.g., "distilbert-en")
    std::string display_name;   // Human-readable name
    std::string model_url;      // URL to download ONNX model
    std::string tokenizer_url;  // URL to download tokenizer config
    std::string tokenizer_type; // "wordpiece" or "openvino"
    std::vector<std::string> languages;  // Supported languages
    size_t model_size_mb;       // Approximate model size in MB
};

/**
 * Get list of all supported NER models
 */
static const std::vector<ModelMetadata>& GetSupportedModels() {
    static std::vector<ModelMetadata> models = {
        {
            "distilbert-en",
            "DistilBERT English (Fast, 66MB)",
            "https://huggingface.co/onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX/resolve/main/onnx/model_quantized.onnx",
            "https://huggingface.co/onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX/resolve/main/tokenizer.json",
            "wordpiece",
            {"en"},
            66
        },
        {
            "xlm-roberta-multi",
            "XLM-RoBERTa Multilingual Quantized (280MB)",
            "https://huggingface.co/Davlan/xlm-roberta-large-finetuned-conll03-english/resolve/main/onnx/model_quantized.onnx",
            "https://huggingface.co/Davlan/xlm-roberta-large-finetuned-conll03-english/resolve/main/tokenizer.json",
            "openvino",  // Will use OpenVINO Tokenizers extension
            {"en", "de", "es", "nl", "fr", "it", "pt", "pl", "ru", "zh", "ja", "ar"},
            280
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

// Current model configuration (thread-safe singleton)
static std::string g_current_model_name = "distilbert-en";
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

    // Parse JSON using yyjson
    yyjson_doc *doc = yyjson_read(json_str.c_str(), json_str.size(), 0);
    if (!doc) {
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Failed to parse tokenizer JSON");
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        AnofoxTrace(AnofoxLogLevel::Error, "ner: Empty tokenizer JSON");
        return false;
    }

    // Navigate to model.vocab
    yyjson_val *model = yyjson_obj_get(root, "model");
    if (!model) {
        yyjson_doc_free(doc);
        AnofoxTrace(AnofoxLogLevel::Error, "ner: No 'model' key in tokenizer JSON");
        return false;
    }

    yyjson_val *vocab = yyjson_obj_get(model, "vocab");
    if (!vocab) {
        yyjson_doc_free(doc);
        AnofoxTrace(AnofoxLogLevel::Error, "ner: No 'model.vocab' key in tokenizer JSON");
        return false;
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

    yyjson_doc_free(doc);

    AnofoxTrace(AnofoxLogLevel::Info, "ner: Loaded vocabulary with " + std::to_string(vocab_.size()) + " tokens");
    return !vocab_.empty();
}

std::vector<std::string> WordPieceTokenizer::Tokenize(const std::string &text) {
    // Basic tokenization: split on whitespace and punctuation
    std::vector<std::string> words;
    std::string current_word;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current_word.empty()) {
                words.push_back(current_word);
                current_word.clear();
            }
        } else if (std::ispunct(static_cast<unsigned char>(c))) {
            if (!current_word.empty()) {
                words.push_back(current_word);
                current_word.clear();
            }
            words.push_back(std::string(1, c));
        } else {
            current_word += c;
        }
    }
    if (!current_word.empty()) {
        words.push_back(current_word);
    }

    return words;
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

    // Tokenize text into words
    std::vector<std::string> words = Tokenize(text);

    // Track position in original text
    size_t text_pos = 0;

    for (const auto &word : words) {
        // Find word position in original text
        size_t word_start = text.find(word, text_pos);
        if (word_start == std::string::npos) {
            word_start = text_pos;  // Fallback
        }
        text_pos = word_start + word.size();

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
    return snapshot;
}

void NERModelManager::SetStatusMessage(const std::string &message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_message_ = message;
}

std::string NERModelManager::GetStatusMessage() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_message_;
}

std::string NERModelManager::DefaultModelPath() {
    // Default path: ~/.duckdb/extensions/anofox/ner/
#ifdef _WIN32
    // Windows: use USERPROFILE environment variable
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        auto path = std::filesystem::path(userprofile) / ".duckdb" / "extensions" / "anofox" / "ner" / "model_quantized.onnx";
        return path.string();
    }
#else
    // Unix-like: use HOME environment variable
    const char* home = std::getenv("HOME");
    if (home) {
        auto path = std::filesystem::path(home) / ".duckdb" / "extensions" / "anofox" / "ner" / "model_quantized.onnx";
        return path.string();
    }
#endif
    return "";
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

    // Double-check after acquiring lock
    if (status_.load() == NERStatus::LOADED) {
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

    const std::string model_path = GetModelPath();
    const std::string device_name = GetDevice();
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        model_path_ = model_path;
    }

    // Check if model file exists
    if (!std::filesystem::exists(model_path)) {
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Model not found, attempting download...");
        status_.store(NERStatus::DOWNLOADING);
        SetStatusMessage("Downloading model from HuggingFace...");

        // Create directory if it doesn't exist
        std::filesystem::path dir_path = std::filesystem::path(model_path).parent_path();
        std::filesystem::create_directories(dir_path);

        if (!DownloadModel(model_path)) {
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
        ov::PartialShape dynamic_shape = {1, ov::Dimension::dynamic()};  // batch=1, seq=dynamic
        std::map<std::string, ov::PartialShape> input_shapes;
        input_shapes["input_ids"] = dynamic_shape;
        input_shapes["attention_mask"] = dynamic_shape;
        model->reshape(input_shapes);

        // Log available devices for diagnostics
        auto available = core_->get_available_devices();
        std::string dev_list;
        for (const auto &d : available) dev_list += d + " ";
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Available OpenVINO devices: " + dev_list);

        // Compile model for configured device (default AUTO: selects GPU if available, falls back to CPU)
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Compiling model for device: " + device_name);
        compiled_model_ = std::make_shared<ov::CompiledModel>(
            core_->compile_model(model, device_name)
        );

        // Log the actual execution device(s) chosen by AUTO or by explicit selection
        auto exec_devices = compiled_model_->get_property(ov::execution_devices);
        std::string exec_dev_str;
        for (size_t di = 0; di < exec_devices.size(); ++di) {
            if (di > 0) exec_dev_str += ",";
            exec_dev_str += exec_devices[di];
        }
        AnofoxTrace(AnofoxLogLevel::Info, "ner: Executing on: " + exec_dev_str);

        // Note: inference requests are created per ExtractEntities call;
        // a single ov::InferRequest is stateful and must not be shared
        // across DuckDB worker threads (issue #50)

        // Create and load tokenizer based on model type
        std::string model_name = GetCurrentModelName();
        auto* metadata = GetModelMetadata(model_name);

        if (metadata && metadata->tokenizer_type == "wordpiece") {
            // DistilBERT uses WordPiece tokenizer (tokenizer.json)
            tokenizer_ = std::make_unique<WordPieceTokenizer>();
            auto tokenizer_path = std::filesystem::path(model_path_).parent_path() / "tokenizer.json";
            if (!tokenizer_->Load(tokenizer_path.string())) {
                AnofoxTrace(AnofoxLogLevel::Warn, "ner: Failed to load tokenizer vocabulary, NER may not work correctly");
            }
        }
#if HAVE_SENTENCEPIECE
        else if (metadata && metadata->tokenizer_type == "openvino") {
            // XLM-RoBERTa uses SentencePiece tokenizer
            tokenizer_ = std::make_unique<SentencePieceTokenizer>();
            auto tokenizer_path = std::filesystem::path(model_path_).parent_path() / "sentencepiece.bpe.model";
            if (!tokenizer_->Load(tokenizer_path.string())) {
                AnofoxTrace(AnofoxLogLevel::Warn, "ner: Failed to load SentencePiece model, NER may not work correctly");
            }
        }
#endif
        else {
            // Default to WordPiece for backward compatibility
            tokenizer_ = std::make_unique<WordPieceTokenizer>();
            auto tokenizer_path = std::filesystem::path(model_path_).parent_path() / "tokenizer.json";
            if (!tokenizer_->Load(tokenizer_path.string())) {
                AnofoxTrace(AnofoxLogLevel::Warn, "ner: Failed to load tokenizer vocabulary, NER may not work correctly");
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            current_model_name_ = model_name;
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

bool NERModelManager::DownloadModel(const std::string &dest_path) {
    // HuggingFace URLs for the DistilBERT NER model
    static const std::string MODEL_URL =
        "https://huggingface.co/onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX/resolve/main/onnx/model_quantized.onnx";
    static const std::string TOKENIZER_URL =
        "https://huggingface.co/onnx-community/distilbert-base-cased-finetuned-conll03-english-ONNX/resolve/main/tokenizer.json";

    std::filesystem::path model_dir = std::filesystem::path(dest_path).parent_path();
    std::string tokenizer_path = (model_dir / "tokenizer.json").string();

    // Download both files
    struct FileToDownload {
        std::string url;
        std::string dest;
        std::string name;
    };

    std::vector<FileToDownload> files = {
        {TOKENIZER_URL, tokenizer_path, "tokenizer.json"},
        {MODEL_URL, dest_path, "model_quantized.onnx"}
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

            // Open file for writing
            FILE *fp = fopen(file.dest.c_str(), "wb");
            if (!fp) {
                AnofoxTrace(AnofoxLogLevel::Error, "ner: Failed to open file for writing: " + file.dest);
                return false;
            }

            // Initialize curl
            CURL *curl = curl_easy_init();
            if (!curl) {
                fclose(fp);
                AnofoxTrace(AnofoxLogLevel::Error, "ner: Failed to initialize curl");
                return false;
            }

            // Configure curl options
            curl_easy_setopt(curl, CURLOPT_URL, file.url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataToFile);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);  // 10 minute timeout for large model
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "anofox-tabular/1.0");

            // Perform download
            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

            curl_easy_cleanup(curl);
            fclose(fp);

            if (res == CURLE_OK) {
                download_success = true;
                AnofoxTrace(AnofoxLogLevel::Info, "ner: Download complete: " + file.name);
            } else {
                const char *error_msg = curl_easy_strerror(res);
                AnofoxTrace(AnofoxLogLevel::Warn,
                    "ner: Download attempt " + std::to_string(attempt) + " failed: " +
                    std::string(error_msg) + " (HTTP " + std::to_string(http_code) + ")");

                // Remove partial file
                std::filesystem::remove(file.dest);

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

    SetStatusMessage("Download complete");
    return true;
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
        if (tokenized.ids.empty()) {
            return {};
        }

        size_t actual_length = tokenized.ids.size();

        // Pad to minimum length to avoid OpenVINO shape inference issues
        // with very short sequences in the quantized model
        static const size_t MIN_SEQ_LENGTH = 16;
        size_t seq_length = std::max(actual_length, MIN_SEQ_LENGTH);

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

        // Run inference
        infer_request.infer();

        // Get output tensor
        ov::Tensor output_tensor = infer_request.get_output_tensor(0);
        const float* logits_data = output_tensor.data<float>();

        std::vector<float> logits(logits_data, logits_data + seq_length * NUM_LABELS);

        auto entities = PostProcess(logits, seq_length, text, tokenized.offsets);

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

std::vector<NEREntity> NERModelManager::ExtractEntities(const std::string &text) {
#if HAVE_OPENVINO
    if (!IsAvailable() || text.empty()) {
        return {};
    }
    return RunSingleInference(text);
#else
    return {};
#endif
}

/**
 * Merge consecutive NER entities of the same type with no gaps.
 * This fixes fragmentation where the model outputs multiple B- tags
 * instead of B- followed by I- tags for multi-token entities.
 *
 * Example: "Donauwoerth" fragmented as [Don, au, wo, ert, h] all tagged as LOCATION
 *          -> merged to single entity "Donauwoerth" with averaged confidence
 *
 * @param entities Input entities (must be sorted by start_pos)
 * @return Merged entities with averaged confidences
 */
static std::vector<NEREntity> MergeConsecutiveSameTypeEntities(
    const std::vector<NEREntity> &entities
) {
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

        // Merge consecutive same-type entities with no gaps
        while (i + 1 < entities.size() &&
               entities[i + 1].label == current.label &&
               entities[i + 1].start_pos == current.end_pos) {
            i++;
            current.text += entities[i].text;
            current.end_pos = entities[i].end_pos;
            total_confidence += entities[i].confidence;
            count++;
        }

        // Average confidence across all merged tokens
        current.confidence = total_confidence / count;
        merged.push_back(current);
        i++;
    }

    return merged;
}

std::vector<NEREntity> NERModelManager::PostProcess(
    const std::vector<float> &logits,
    size_t seq_length,
    const std::string &text,
    const std::vector<std::pair<size_t, size_t>> &offsets
) {
    std::vector<NEREntity> entities;
    const auto& labels = GetLabels();

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
        const float* pos_logits = &logits[i * NUM_LABELS];

        // Apply softmax and find max
        float max_logit = *std::max_element(pos_logits, pos_logits + NUM_LABELS);
        float sum_exp = 0.0f;
        for (int j = 0; j < NUM_LABELS; ++j) {
            sum_exp += std::exp(pos_logits[j] - max_logit);
        }

        int best_label_idx = 0;
        float best_prob = 0.0f;
        for (int j = 0; j < NUM_LABELS; ++j) {
            float prob = std::exp(pos_logits[j] - max_logit) / sum_exp;
            if (prob > best_prob) {
                best_prob = prob;
                best_label_idx = j;
            }
        }

        std::string label = labels[best_label_idx];

        // Check if this is a B- (begin) or I- (inside) tag
        bool is_begin = label.size() > 2 && label[0] == 'B' && label[1] == '-';
        bool is_inside = label.size() > 2 && label[0] == 'I' && label[1] == '-';
        std::string entity_type = (is_begin || is_inside) ? label.substr(2) : "";

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
    AnofoxTrace(AnofoxLogLevel::Info,
                "ner: Device set to '" + device_name + "' (reload required for changes to take effect)");
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
        if (!texts[i].empty()) {
            // Empty texts keep their default empty result
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

    // Validate model name
    if (!GetModelMetadata(model_name)) {
        throw InvalidInputException(
            "Unsupported NER model: " + model_name + ". Valid models: " + GetValidModelNames());
    }

    // Store the new model name
    SetCurrentModelName(model_name);

    AnofoxTrace(AnofoxLogLevel::Info,
                "ner: Model set to '" + model_name + "' (reload required for changes to take effect)");

    // Note: Actual model reload will happen on next EnsureInitialized() call
    // TODO: Implement NERModelManager::ReloadModel() for immediate reload
}

void SetNERDeviceOption(ClientContext &context, SetScope scope, Value &parameter) {
    if (parameter.IsNull()) {
        throw InvalidInputException("anofox_ner_device cannot be NULL");
    }
    auto device_name = StringValue::Get(parameter);
    NERModelManager::Instance().SetDevice(device_name);
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
                              "NER model to use: distilbert-en (fast, English) or xlm-roberta-multi (multilingual)",
                              LogicalTypeId::VARCHAR,
                              Value("distilbert-en"),
                              SetNERModelOption);

    config.AddExtensionOption("anofox_ner_device",
                              "OpenVINO device for NER inference: AUTO (default), CPU, GPU, GPU.0, etc.",
                              LogicalTypeId::VARCHAR,
                              Value("AUTO"),
                              SetNERDeviceOption);
}

} // namespace anofox
} // namespace duckdb
