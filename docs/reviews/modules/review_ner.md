## Summary
The NER module has a workable high-level shape, but the singleton model manager is not safe under DuckDB parallel execution. The most serious risks are shared mutable tokenizer/OpenVINO inference state and cache races that can produce data races, corrupted offsets, undefined behavior, or wrong results. Model configuration is also only partially wired: advertised multilingual/model-selection support does not actually download or load distinct model assets reliably. Performance is acceptable for small inputs, but the current implementation leaves significant row-by-row allocation and tokenization overhead on the hot path.

## Findings

### HIGH shared inference and tokenizer state is not thread-safe
Affected: `src/anofox_ner.cpp:701`, `src/anofox_ner.cpp:722`, `src/anofox_ner.cpp:727`, `src/anofox_ner.cpp:757`, `src/anofox_ner.cpp:761`, `src/anofox_ner.cpp:1051`, `src/anofox_ner.cpp:1056`, `src/anofox_ner.cpp:1085`, `src/anofox_ner.cpp:1089`, `src/include/anofox_ner.hpp:198`, `src/include/anofox_ner.hpp:199`, `src/include/anofox_ner.hpp:348`, `src/include/anofox_ner.hpp:352`

`NERModelManager` is a process-wide singleton, and DuckDB scalar/table function execution can call into it from multiple worker threads. `ExtractEntities` and `ExtractEntitiesBatch` mutate shared `tokenizer_` state via `Encode`, then read `GetOffsets`, while another thread can clear and replace `offsets_` before post-processing. The same functions also reuse one shared `ov::InferRequest`; OpenVINO infer requests are stateful objects and should not be concurrently `set_tensor`/`infer`-ed from multiple threads.

Suggested improvement: make tokenization output offsets by value and use per-call/per-thread inference requests, or guard the whole tokenize/infer path with a mutex if correctness is preferred over throughput.

```cpp
struct TokenizedInput {
    std::vector<int64_t> ids;
    std::vector<std::pair<size_t, size_t>> offsets;
};

virtual TokenizedInput EncodeWithOffsets(std::string_view text) const = 0;

// Prefer thread-local/request-local infer request:
auto request = compiled_model_->create_infer_request();
request.set_tensor("input_ids", input_ids_tensor);
request.set_tensor("attention_mask", attention_mask_tensor);
request.infer();
```

### HIGH LRU cache has data races and capacity-zero undefined behavior
Affected: `src/include/anofox_ner.hpp:61`, `src/include/anofox_ner.hpp:73`, `src/include/anofox_ner.hpp:74`, `src/include/anofox_ner.hpp:104`, `src/include/anofox_ner.hpp:109`, `src/anofox_ner.cpp:708`, `src/anofox_ner.cpp:772`, `src/anofox_ner.cpp:1013`, `src/anofox_ner.cpp:1100`, `src/anofox_ner.cpp:1139`

`LRUCache::Capacity()` reads `capacity_` without locking while `SetCapacity()` writes it under a mutex, which is a C++ data race. The call sites check `Capacity() > 0` before `Put`, but that check is outside the cache lock; if another thread sets capacity to zero between the check and `Put`, `Put` can execute `lru_list_.back()` on an empty list because `cache_.size() >= capacity_` is true when capacity is zero. This is undefined behavior and can crash when users change `anofox_ner_cache_size` concurrently with queries.

Suggested improvement: make `Capacity()` lock or make `capacity_` atomic, and make `Put` handle zero capacity internally while holding the mutex.

```cpp
void Put(const Key &key, const Value &value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity_ == 0) {
        return;
    }
    ...
}
```

### MEDIUM model selection is advertised but not actually implemented end-to-end
Affected: `src/anofox_ner.cpp:73`, `src/anofox_ner.cpp:87`, `src/anofox_ner.cpp:437`, `src/anofox_ner.cpp:546`, `src/anofox_ner.cpp:596`, `src/anofox_ner.cpp:613`, `src/anofox_ner.cpp:1142`, `src/anofox_ner.cpp:1155`, `src/anofox_ner.cpp:1160`

The module exposes `anofox_ner_model` and metadata for `xlm-roberta-multi`, but `GetModelPath()` always resolves to the same `model_quantized.onnx`, `DownloadModel()` always uses hard-coded DistilBERT URLs, and `EnsureInitialized()` returns immediately once any model is loaded. Setting the option after load logs “reload required” but there is no reload path, and setting it before load still downloads the DistilBERT assets while selecting tokenizer behavior from the configured metadata. This can lead to mismatched tokenizer/model pairs or a setting that silently has no effect.

Suggested improvement: derive model/tokenizer paths and download URLs from `ModelMetadata`, store each model in a distinct directory, and either reject changing the option after load or implement an atomic reload that clears the cache and replaces model/tokenizer state under a lock.

### MEDIUM status and path fields are read without synchronization
Affected: `src/anofox_ner.cpp:433`, `src/anofox_ner.cpp:437`, `src/anofox_ner.cpp:500`, `src/anofox_ner.cpp:506`, `src/anofox_ner.cpp:631`, `src/anofox_ner.cpp:697`, `src/anofox_ner.cpp:576`, `src/anofox_pii.cpp:2522`, `src/anofox_pii.cpp:2540`, `src/anofox_pii.cpp:2546`

`status_` is atomic, but related fields such as `status_message_`, `model_path_`, and `current_model_name_` are plain strings accessed from status table functions and query paths while model loading/download updates them. Concurrent reads/writes of `std::string` are data races. `anofox_ner_status()` can therefore observe torn or invalid string state while another thread initializes the model.

Suggested improvement: protect status metadata with a mutex, or publish an immutable status snapshot object after each update.

### MEDIUM long inputs are not bounded to model limits
Affected: `src/anofox_ner.cpp:722`, `src/anofox_ner.cpp:733`, `src/anofox_ner.cpp:745`, `src/anofox_ner.cpp:767`, `src/anofox_ner.cpp:1051`, `src/anofox_ner.cpp:1061`, `src/anofox_ner.cpp:1073`, `src/anofox_ner.cpp:1095`

The tokenizer can emit arbitrary-length sequences, and the code reshapes the OpenVINO input to that length. BERT-style NER models usually have a fixed maximum positional embedding length, commonly 512 tokens; longer text may fail inference, return empty results after an exception, or allocate very large intermediate tensors/vectors. The code also assumes the output buffer contains exactly `seq_length * NUM_LABELS` floats without checking the actual output shape.

Suggested improvement: enforce the model’s maximum sequence length, preferably with a sliding-window strategy if full-text coverage is needed, and validate the output tensor shape before copying logits.

```cpp
constexpr size_t MAX_SEQ_LENGTH = 512;
if (input_ids.size() > MAX_SEQ_LENGTH) {
    input_ids.resize(MAX_SEQ_LENGTH);
    offsets.resize(MAX_SEQ_LENGTH);
    input_ids.back() = SEP_TOKEN_ID;
}
```

### MEDIUM tokenizer implementation does not match tokenizer.json semantics
Affected: `src/anofox_ner.cpp:150`, `src/anofox_ner.cpp:212`, `src/anofox_ner.cpp:249`, `src/anofox_ner.cpp:263`, `src/anofox_ner.cpp:287`, `src/anofox_ner.cpp:301`

`WordPieceTokenizer::Load` only reads `model.vocab`; it ignores the normalizer, pre-tokenizer, post-processor, unknown-token policy, and truncation settings from `tokenizer.json`. The hand-written tokenizer splits on bytes with `std::isspace`/`std::ispunct`, so UTF-8 punctuation and non-ASCII text can be split into invalid byte fragments and mapped poorly to offsets. This is not just a quality issue: downstream PII spans can become incorrect when token offsets do not correspond to model tokenization.

Suggested improvement: use a tokenizer implementation that consumes the full HuggingFace tokenizer configuration, or explicitly constrain this module to ASCII/English and document/test the offset behavior. If keeping this implementation, return offsets as part of tokenization and add UTF-8 aware pre-tokenization.

### LOW `DownloadModel` uses C resources manually and can leave partial asset sets
Affected: `src/anofox_ner.cpp:633`, `src/anofox_ner.cpp:641`, `src/anofox_ner.cpp:665`, `src/anofox_ner.cpp:666`, `src/anofox_ner.cpp:677`

The download path uses raw `FILE*` and `CURL*` with manual cleanup. The current control flow closes both on the visible paths, but RAII would make this safer as the function grows. Also, tokenizer and model files are written directly to their final paths; a failure after the tokenizer download but before model completion can leave a stale tokenizer paired with a missing or later-replaced model.

Suggested improvement: wrap `FILE*`/`CURL*` in `unique_ptr` with custom deleters and download to temporary files before atomically renaming completed assets.

### LOW duplicated inference setup increases maintenance risk
Affected: `src/anofox_ner.cpp:716`, `src/anofox_ner.cpp:744`, `src/anofox_ner.cpp:769`, `src/anofox_ner.cpp:1046`, `src/anofox_ner.cpp:1072`, `src/anofox_ner.cpp:1097`

`ExtractEntities` and `ExtractEntitiesBatch` duplicate nearly the same tokenization, padding, tensor creation, inference, logits copy, post-process, and cache-store logic. This makes concurrency fixes, shape checks, max-length handling, and output validation easy to apply in one path but miss in the other. The `max_batch_size` parameter is also currently unused, so the batch API name overstates what it does.

Suggested improvement: factor a private `RunSingleInference(text)` helper that returns `std::vector<NEREntity>`, and have both single and batch paths share it. Either remove `max_batch_size` or implement true batched inference.

## Quick wins
- Add a mutex around `ExtractEntities` and `ExtractEntitiesBatch` immediately to stop shared tokenizer/infer-request races while a better per-thread request design is built.
- Fix `LRUCache::Put` to return when `capacity_ == 0`, and make `Capacity()` synchronized.
- Add an early `if (text.empty()) return {};` in `ExtractEntities` to match the batch path and avoid unnecessary inference.
- Validate output tensor shape before constructing `std::vector<float>` from `logits_data`.
- Replace hard-coded download URLs with `ModelMetadata` fields, or remove the unsupported multilingual option until reload and asset management are implemented.
- Factor the duplicated single-row inference code into one private helper before adding truncation, shape checks, or per-thread request handling.