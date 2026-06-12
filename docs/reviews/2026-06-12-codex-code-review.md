# Consolidated Code Review — anofox-tabular

- **Date:** 2026-06-12
- **Reviewer:** OpenAI Codex (gpt-5.5), one independent read-only review per module, consolidated by Claude Code
- **Scope:** All C++ functionality modules in `src/` (email, postal, phonenumber, metric/quality, outlier detection, diff, money, PII, NER, VAT, core/trace/telemetry)
- **Goal:** Input for planning code-quality improvements: correctness, performance/memory, source-code modelling
- **Detail:** Full per-module reviews with code sketches live in `docs/reviews/modules/review_<module>.md`

**Finding counts:** 23 HIGH, 37 MEDIUM, 19 LOW across 11 modules.

---

## 1. Cross-module themes

These recurring patterns account for the majority of findings. Fixing them centrally (shared helper + sweep) gives far more leverage than patching module by module.

### T1 — SQL built by string concatenation without escaping (HIGH)
**Modules:** metric, outlier_tree, pii, diff.
Table names, column names, and string literals are interpolated into generated SQL. Quotes or keywords in valid identifiers break the query; crafted input can alter it (SQL injection through `query_table('...')`, `pii_scan_table`, `outlier_tree`, diff join/compare columns).
**Fix:** one shared header with `QuoteIdentifier()` / `QuoteLiteral()` (or DuckDB's `KeywordHelper::WriteOptionallyQuoted`), used by every SQL-generating path. Add sqllogictests with quoted/`'`-containing identifiers.

### T2 — Process-global mutable singletons without consistent synchronization or scoping (HIGH)
**Modules:** email (options), phonenumber (default region), pii (`PIIConfig`, Base58 map), ner (model manager, LRU cache), money (`CurrencyRegistry` init), postal (`PostalManager` fields), core (telemetry `_queue`/`_api_key`).
Concrete data races exist (unsynchronized lazy init, plain-`bool`/`std::string` reads while another thread writes). Beyond races, settings are process-wide, so one connection's `SET` changes another connection's running query, and `FunctionStability::CONSISTENT` is claimed for functions whose results depend on mutable global state.
**Fix pattern:** (a) make one-time init safe via constructor-init or `std::call_once`; (b) snapshot settings per query into `FunctionData` at bind time instead of reading the singleton per row; (c) audit every singleton so all reads/writes of non-atomic fields hold the same mutex; (d) re-evaluate `FunctionStability` flags.

### T3 — Per-row work that belongs at chunk/registry level (MEDIUM, perf)
**Modules:** phonenumber and vat compile `std::regex` per row; email calls `ToUnifiedFormat` per row; money/vat/postal/pii copy every `string_t` into `std::string`; money calls `StructVector::GetEntries` inside row loops.
**Fix:** precompile regexes once (registry/metadata level); hoist vector normalization out of loops; pass `string_t`/`string_view` into row lambdas; use `UnaryExecutor`/`BinaryExecutor`/`GenericExecutor` for simple scalar functions to preserve constant/dictionary vector handling.

### T4 — Table functions fully materialize input and output (MEDIUM, memory)
**Modules:** metric (isolation forest path), outlier_tree, pii (`pii_scan_table`, `pii_audit_table`).
Whole source tables are fetched through a nested `Connection`, copied cell-by-cell via `DataChunk::GetValue`, plus a second full copy of results before any row is emitted.
**Fix:** stream output per `STANDARD_VECTOR_SIZE` chunk from table-function state; read input through `UnifiedVectorFormat`; avoid the duplicate results vector; document/guard unavoidable full-materialization (model fitting).

### T5 — Parameter validation gaps (MEDIUM)
**Modules:** metric, outlier, phonenumber, ner, email.
Signed `BIGINT` inputs are assigned to `size_t` before range checks (`min_pts = -1` wraps to huge), `NaN`/`Inf` pass `<=`/`>` checks, unknown enum-ish strings silently fall back to defaults (`output_mode`, phone format option), and settings accept invalid values (`default_region='ZZ'`, DNS tries > `INT_MAX`).
**Fix:** validate into `int64_t` first then cast; `std::isfinite()` checks; reject unknown option strings with `BinderException`/`InvalidInputException`; validate settings in their `SET` callbacks.

### T6 — STRUCT-result NULL semantics are inconsistent (HIGH for postal, MEDIUM for money)
`postal_parse_address(NULL)` and invalid money rows mark only child vectors NULL, leaving a non-NULL parent struct — `f(NULL) IS NULL` is false.
**Fix:** define one invariant (invalid ⇒ parent struct NULL) and apply it via a small shared helper; add tests.

### T7 — Functions that don't do what their name/signature promises (HIGH)
- metric: `dbscan`/`dbscan_mv` are bind-replace **placeholders** returning constants, despite a real `DBSCAN` class existing.
- vat: `vat_format` always returns NULL; `vat_is_valid` is syntax-only despite modeling `has_checksum`.
- diff: `diff_hashdiff` accepts `bisection_threshold`/`bisection_factor` and ignores both (runs the same join as joindiff).
- ner: `anofox_ner_model` option exists but model paths/URLs are hard-coded; no reload path. `ExtractEntitiesBatch(max_batch_size)` is not batched.
- money: `money_from_cents(10050,'USD')` returns `10050.0`, not `100.50` — `subunit_to_unit` is fetched but unused.
**Fix:** implement, or unregister/rename until implemented. Silent stubs are worse than missing functions.

### T8 — `<cctype>` UB on signed char (LOW, trivial sweep)
**Modules:** pii, vat (ner tokenizer relatedly). `std::isdigit(c)` etc. with plain `char` is UB for negative values. **Fix:** sweep to `static_cast<unsigned char>`.

### T9 — Dead/legacy code paths increase review risk (LOW)
metric keeps deprecated isolation-forest SQL generators; diff carries a complete unregistered native table-function scaffold that would return empty results if wired up. **Fix:** delete.

### T10 — Alias helpers silently drop function metadata (MEDIUM, affects all modules)
`anofox_function_alias.hpp` reconstructs `ScalarFunction`/`TableFunction` copying only a subset of fields — aliases lose `init_local_state`, statistics/pushdown callbacks, error mode, serialization, etc., and will drift further as DuckDB evolves.
**Fix:** copy the whole function object, then rename (`auto alias = func; alias.name = alias_name;`); add primary-vs-alias regression tests.

---

## 2. Per-module findings

### 2.1 Email (`anofox_email*.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | SMTP validation can succeed after DNS failure: fallback hosts include the raw domain, `localhost`, and `127.0.0.1`, so a local SMTP server makes invalid domains validate. Also surprising local network probing. | `anofox_email.cpp:86-88,317-321` |
| HIGH | `FD_SET` with descriptors ≥ `FD_SETSIZE` is UB (memory corruption) in DNS and SMTP `select()` loops. Replace with `poll()`/`WSAPoll()` or guard fd values. | `anofox_email_dns.cpp:188-195`, `anofox_email_smtp.cpp:155-165,350-357` |
| MED | Null MX (`"."` exchange, RFC 7505) treated as deliverable MX host; DNS mode reports such domains valid. Detect and return `dns_null_mx`. | `anofox_email_dns.cpp:121-124,426,434` |
| MED | `ReadLine` has no max line length / total response size / multi-line count — a hostile peer can grow memory unboundedly. Enforce protocol limits (e.g. 8 KiB line / 64 KiB response). | `anofox_email_smtp.cpp:607-670` |
| MED | Overall SMTP timeout only checked between hosts; one host with many endpoints can exceed `MAX_TOTAL_TIMEOUT_MS` by a wide margin. Thread an absolute deadline through `AttemptHost`/`ConnectSocket`/`ReadResponse`/`SendCommand`. | `anofox_email_smtp.cpp:927-935` |
| MED | Per-row `args.GetValue()` + `std::string` conversion + repeated `ToUnifiedFormat` in `ExtractValidationMode`. Normalize once per chunk; vectorize the regex-only path. | `anofox_email.cpp:420-481` |
| LOW | Options are a process-global singleton — one connection's `SET` changes another's running query (theme T2). Snapshot into bind data. | `anofox_email.cpp:92-99,674-676` |
| LOW | DNS tries option accepts up to `uint32_t::max()` then casts to `int` for c-ares — can wrap negative. Cap to a small documented range. | `anofox_email.cpp:568-573`, `anofox_email_dns.cpp:363` |

**Quick wins:** drop localhost fallback; Null-MX handling; guard/replace `FD_SET`; SMTP size limits; RAII for sockets/c-ares channels; reconsider `CONSISTENT` stability on network-dependent overloads.

### 2.2 Postal (`anofox_postal.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | `postal_parse_address(NULL)` returns a non-NULL struct with all-NULL children — parent validity never set (theme T6). | `anofox_postal.cpp:323-325` |
| HIGH | User-controlled data path interpolated into `std::system("tar -xzf \"...\" -C \"...\"")` — shell metacharacter injection. Replace with non-shell extraction (library or argv-based process spawn). | `anofox_postal.cpp:219-220,492` |
| MED | `LoadData` bypasses `init_lock`: concurrent `postal_load_data()` calls can clobber each other's downloads; per-call `curl_global_init`/`cleanup` affects other curl users process-wide. Serialize + process-lifetime curl RAII. | `anofox_postal.cpp:127-227,472` |
| MED | Partial libpostal initialization not rolled back on failure — retry can re-setup already-initialized global state; destructor tears down stages that never completed. Track stages / RAII guard. | `anofox_postal.cpp:263-276` |
| MED | `data_directory` written under mutex but read without it (`GetDataDirectory`, `LoadData`, `GetStatus`) — `std::string` data race. Lock all accessors. | `anofox_postal.hpp:52-53`, `anofox_postal.cpp:74-235` |
| LOW | Error message says `anofox_postal_data_path` but the registered option is `anofox_tab_postal_data_path`; disabled tests use the old name too. | `anofox_postal.cpp:293-295,492` |
| LOW | Per-row intermediates: input→`std::string`, libpostal components→vector→`Value` copies. Write directly into DuckDB vectors with RAII for libpostal responses. | `anofox_postal.cpp:88-118,330-386` |

**Quick wins:** parent struct validity fix; remove duplicate `RegisterPostalOptions` call; fix option-name mismatch; mutex around `LoadData`; RAII for `FILE*`/`CURL*`.

### 2.3 Phonenumber (`anofox_phonenumber*.cpp/.hpp`)

Note: this is a custom implementation, not libphonenumber-backed (README wording may be stale).

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | Process-wide `default_region` mutable via `SET` mid-query; functions marked `CONSISTENT` despite depending on it (theme T2). Snapshot via bind data. | `anofox_phonenumber.cpp:504,511,560,971` |
| HIGH | Region hints ignored for shared country codes: `+1` always resolves to first region (`US`), so `phonenumber_is_valid_for_region('+1 506…','CA')` fails. Prefer the hint when it belongs to the code's region list. | `anofox_phonenumber.cpp:214`, `anofox_phonenumber_metadata.cpp:8` |
| MED | `ExtractCountryCode` consumes 1–3 leading digits even without `+`, so national numbers whose prefix matches a country code (e.g. US area code 442 → GB) are misparsed and never fall back to the region hint. Only extract country code for explicit international formats. | `anofox_phonenumber.cpp:52,177,180` |
| MED | Up to 3 `std::regex` objects compiled per row in `DeterminePhoneNumberType`, and again in `IsValid` after `Parse` already classified. Precompile per region; share classification. | `anofox_phonenumber.cpp:96-113,374-379` |
| LOW | Unknown format strings silently become `NATIONAL`; formatting failures return the input via blanket `catch (std::exception)`. Reject unknown formats; decide NULL-vs-throw for invalid numbers. | `anofox_phonenumber.cpp:529,669-674` |
| LOW | `SET anofox_tab_phonenumber_default_region='ZZ'` accepted silently; later parsing quietly fails. Validate against `REGION_METADATA` in the SET callback. | `anofox_phonenumber.cpp:504,560` |

**Quick wins:** NANPA/CA regression tests; per-region precompiled regexes; validate region setting; strict format option.

### 2.4 Metric / Quality (`anofox_metric.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | Table/column names, required columns, interval strings concatenated into generated SQL without escaping (theme T1). | `anofox_metric.cpp:56-255,653,699,1265` |
| HIGH | `anofox_tab_dbscan`/`dbscan_mv` are placeholder bind-replace SQL: constant `cluster_id=0`, `point_type='CORE'`, hard-coded `cluster_count=3`, `noise_count=1`, `total_count=10` — the real `DBSCAN` class is included but never called. Implement as real table function like `IsolationForestExecute`, or unregister. | `anofox_metric.cpp:1242-1345` |
| MED | Bind functions contain defaults (`n_trees=100`…) but registration only adds fixed-arity overloads — `isolation_forest('t','x')` can never bind. Register all intended arities or delete dead defaulting code. | `anofox_metric.cpp:414,508,1155-1217,1434-1472,1569` |
| MED | Empty/degenerate inputs: `0/0` null_rate, zscore returns zero rows on empty input (cross join with empty CTE), NULL freshness messages, `STDDEV` NULL/0 unhandled. Guarantee exactly one row with explicit semantics (`NULLIF`, `COALESCE`, stddev special-case). | `anofox_metric.cpp:70-130,151,229` |
| MED | `BIGINT`→`size_t` before validation (`min_pts=-1` wraps and passes); NaN passes `eps <= 0.0` / `contamination > 0.5` checks (theme T5). | `anofox_metric.cpp:415-424,509-518,581,1253-1254,1324-1325` |
| MED | Isolation forest materializes the full input via per-cell `GetValue`, full scores vector, plus a duplicate `state.results` copy (theme T4). | `anofox_metric.cpp:701-872` |
| LOW | Any `output_mode` other than exact `"scores"`/`"clusters"` silently means summary — typos change behavior. Reject unknown modes at bind. | `anofox_metric.cpp:440,599,891,1173,1235,1268,1338` |
| LOW | Dead legacy isolation-forest SQL generators and bind-replace functions remain alongside the live C++ path (theme T9). | `anofox_metric.cpp:944-1181` |

**Quick wins:** quoting helpers; strict `output_mode`; signed validation then cast; reject NaN/Inf; tests for quoted identifiers, empty tables, all-NULL columns, constant z-score input.

### 2.5 Outlier detection (`anofox_dbscan.cpp`, `anofox_isolation_forest.cpp`, `anofox_outlier_tree.cpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | DBSCAN semantics wrong: `minPts` effectively one stricter (self excluded from `RegionQuery` but compared directly), and noise points reachable from a core point stay noise instead of becoming border points (skipped by the `visited` check). | `anofox_dbscan.cpp:50,62,97,105,161` |
| HIGH | `outlier_tree` builds SQL from unescaped table/column names and runs it on a nested `Connection` (theme T1). | `anofox_outlier_tree.cpp:795-807,909-912` |
| HIGH | `max_depth`/`min_size_*` read as `int64_t` but assigned to `size_t` before validation — negative values wrap and pass (theme T5). | `anofox_outlier_tree.cpp:823-826` |
| MED | Reported `row_id` is the index into the NULL-filtered compacted dataset, not the source row; `total_rows` is the filtered count. Carry original row ids through (e.g. `row_number() over ()`). | `anofox_outlier_tree.cpp:941-990` |
| MED | Refitting an `IsolationForest` appends to old `nodes_` (never cleared, `trees_.resize` reuses instances) — second fit scores through stale tree state. Clear per build. | `anofox_isolation_forest.cpp:385,517,955,1176` |
| MED | Categorical right-branch explanations lack negation: outliers explained as `col IN (...)` when the branch means `NOT IN`. Add an operator/negation flag to `SplitCondition`. | `anofox_outlier_tree.cpp:154-157`, hpp:40,56 |
| MED | Duplicate-outlier suppression appends a better-scoring duplicate without removing the worse one — inflated counts. Keep a `(row,col)`→best map. | `anofox_outlier_tree.cpp:239,314,736` |
| MED | `SplitCondition::ToJSON` writes unescaped strings into JSON — malformed output for quotes/control chars. | `anofox_outlier_tree.hpp:56-66,120` |
| MED | `Fit`/`Score`/`FitMixed` assume consistent shapes; violations can underflow `uniform_int_distribution(0, n_features-1)` or read out of bounds. Validate at API boundaries. | `anofox_isolation_forest.cpp:393-499,1126-1278` |
| MED | Full-table materialization through nested `Connection` + per-cell `GetValue`/`SetValue` (theme T4). | `anofox_outlier_tree.cpp:915-990` |
| LOW | `uint16_t` feature indices/depths, `uint8_t` ndim — silent truncation on wide tables. Use `idx_t` or validate before narrowing. | `anofox_isolation_forest.hpp:108-117` |
| LOW | Hot-path waste: per-candidate variance/entropy recomputation, double partitioning (score then recurse), fresh neighbor vector per region query. Cache stats, reuse scratch buffers; consider spatial index for DBSCAN. | `anofox_outlier_tree.cpp:120-125,382,398`, `anofox_isolation_forest.cpp:112,139`, `anofox_dbscan.cpp:50` |

**Quick wins:** DBSCAN minPts/border regression tests; clear `nodes_` per build; JSON escaping; categorical negation flag; preserve source row ids; signed-validate-then-cast.

### 2.6 Diff (`anofox_diff.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | Nullable primary keys break classification: join on `s.pk = t.pk` plus presence inferred from `s.<first_pk> IS NULL` misclassifies NULL-key and compound-key rows as added/removed. Use side-presence marker columns + `IS NOT DISTINCT FROM`, or reject NULL PKs explicitly. | `anofox_diff.cpp:228,237,254-255` |
| MED | PK/compare column identifiers concatenated unquoted (theme T1); table names go through `QualifiedName` but columns don't. Use `KeywordHelper::WriteOptionallyQuoted`. | `anofox_diff.cpp:228-278` |
| MED | `ValidatePrimaryKeys`/`ValidateCompareColumns` exist but are never called; schema mismatches surface as confusing binder errors or silently wrong whole-row-struct comparisons. Validate at bind; compare column-by-column. | `anofox_diff.cpp:82,97,259-277` |
| MED | `diff_hashdiff` registers `bisection_threshold`/`bisection_factor` overloads and ignores both — same plan as joindiff (theme T7). Implement or reject. | `anofox_diff.cpp:382-388,521-533` |
| LOW | Complete unregistered native table-function scaffold (would return empty results) plus unused constants/helpers (theme T9). Delete or finish. | `anofox_diff.cpp:22-189` |
| LOW | Manual alias copying instead of `RegisterTableFunctionSetWithAlias` from `anofox_function_alias.hpp`. | `anofox_diff.cpp:400,484,507,561` |

### 2.7 Money (`anofox_money*.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | `CurrencyRegistry::GetInstance()` does an unsynchronized `if (!initialized) Initialize()` after the static — two threads can mutate `currencies` concurrently. Init in constructor or `std::call_once`. | `anofox_money_currency.cpp:7-15`, hpp:51-52 |
| HIGH | `money_from_cents(10050,'USD')` returns `10050.0 USD` — `subunit_to_unit` fetched but never divided by (theme T7). Also wrong for zero-decimal currencies. | `anofox_money.cpp:70,101-108` |
| HIGH | Amounts modeled as `DOUBLE`: inexact arithmetic, NaN/Inf representable, integer precision loss above 2^53. Move to `BIGINT` minor units or `DECIMAL(18,2)` with overflow checks. | `anofox_money.cpp:19-21,364-630`, hpp:18,45 |
| MED | Functions that throw (`InvalidInputException` on bad currency / mismatch) registered with default `CANNOT_ERROR` — planner may treat them as non-throwing. `SetFallible()` everywhere applicable. | `anofox_money.cpp:57-220,619`, hpp:154 |
| MED | Child-field NULLs produce a valid parent money struct (theme T6); define one invariant for accessors/comparisons. | hpp:144-148, `anofox_money.cpp:343,408` |
| MED | Case-insensitive lookup accepts `'usd'` but stores original string; later case-sensitive comparison makes `money_add(money(1,'usd'), money(1,'USD'))` throw. Canonicalize to `iso_code` at construction. | `anofox_money.cpp:54-61`, hpp:151,154 |
| LOW | `money_format` ignores `subunit_to_unit` scale (JPY → `¥1000.00`), never inserts `thousands_separator`, can emit `nan`/`inf`. Centralize a metadata-driven formatter. | `anofox_money.cpp:227-253` |
| LOW | Per-row `GetString()` copies, `StructVector::GetEntries` inside row loops (theme T3). | hpp:71-151, `anofox_money.cpp:153-169,365` |

### 2.8 PII (`anofox_pii.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | `PIIConfig` singleton: mutable vectors/doubles/enums read by parallel workers while SET callbacks write, no mutex, no `ClientContext` scoping (theme T2). Snapshot per query. | `anofox_pii.cpp:106-147,3153,3182`, hpp:507,537 |
| HIGH | Lazy Base58 map populated on `empty()` check — classic data race for parallel crypto-address validation. Use immutable static array initialized by lambda. | `anofox_pii.cpp:992-998` |
| HIGH | `pii_scan_table`/`pii_audit_table` execute SQL rebuilt from the raw table name; column quotes unescaped (theme T1). | `anofox_pii.cpp:2638,2695,2853,2929` |
| MED | `anofox_pii_enabled_types` ignored by main scalar APIs (`engine.Detect(text)` without config); `min_confidence` only applied in audit; NER recognizers hard-code 0.7. Centralize filtering in `Detect()`. | `anofox_pii.cpp:147-169,1627,1778,1875,2727-2730,3231` |
| MED | Overlapping matches from multiple recognizers only sorted by start — `Mask()` can double-mask spans with stale offsets. Resolve overlaps by priority/length before masking. | `anofox_pii.cpp:1627-1650,1726-1734` |
| MED | Both table functions materialize all results before emitting; audit retains full original+masked values per match (theme T4). Stream chunks. | `anofox_pii.cpp:2654,2742,2891,2962,2977` |
| LOW | Multiple `std::isdigit`/`isalpha` calls on plain `char` (theme T8) — some sites already cast, this is consistency debt. | `anofox_pii.cpp:311-560` |

### 2.9 NER (`anofox_ner.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | Shared mutable tokenizer state (`offsets_` cleared/replaced across threads) and a single shared `ov::InferRequest` used concurrently — data races / wrong results under parallel execution. Per-call infer requests + tokenization returning offsets by value (or a mutex as stopgap). | `anofox_ner.cpp:701-761,1051-1089`, hpp:198-199,348-352 |
| HIGH | `LRUCache::Capacity()` reads unlocked while `SetCapacity()` writes; capacity-zero between check and `Put` ⇒ `lru_list_.back()` on empty list (UB/crash when `anofox_ner_cache_size` changes mid-query). Lock/atomic + handle zero inside `Put`. | hpp:61-109, call sites 708-1139 |
| MED | `anofox_ner_model` advertised (incl. `xlm-roberta-multi`) but paths/URLs hard-coded to DistilBERT; no reload; mismatched tokenizer/model pairs possible (theme T7). Drive assets from `ModelMetadata` or reject the option. | `anofox_ner.cpp:73-87,437,546-613,1142-1160` |
| MED | `status_message_`, `model_path_`, `current_model_name_` are plain strings read by status functions while loading threads write — torn reads. Publish immutable snapshots. | `anofox_ner.cpp:433-506,631,697`, `anofox_pii.cpp:2522-2546` |
| MED | No max-sequence-length enforcement (BERT-class models ~512 tokens) and output tensor shape never validated before copying logits. Truncate/window + shape check. | `anofox_ner.cpp:722-767,1051-1095` |
| MED | Hand-written WordPiece ignores tokenizer.json normalizer/pre-tokenizer settings; byte-level `isspace`/`ispunct` splitting breaks UTF-8 → wrong offsets → wrong PII spans. Use full tokenizer config or constrain+document ASCII. | `anofox_ner.cpp:150-301` |
| LOW | Raw `FILE*`/`CURL*` with manual cleanup; assets written to final paths (partial downloads leave mismatched pairs). RAII + temp-file + atomic rename. | `anofox_ner.cpp:633-677` |
| LOW | `ExtractEntities`/`ExtractEntitiesBatch` duplicate the full pipeline; `max_batch_size` unused. Factor `RunSingleInference()`. | `anofox_ner.cpp:716-769,1046-1097` |

### 2.10 VAT (`anofox_vat*.cpp/.hpp`)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | `vat_format` is a registered public function that always returns NULL (theme T7). Implement using `SplitVAT`/`ConvertISOToVAT` or unregister. | `anofox_vat.cpp:128-151,286` |
| HIGH | `vat_is_valid` is syntax-only although the country table models `has_checksum` for most countries — significant false-positive risk. Add checksum implementations; split syntax vs full validation in the API. | `anofox_vat_country.cpp:25,57`, hpp:141-143, `anofox_vat.cpp:161-172` |
| MED | Country utilities do raw map lookups: `is_valid_vat_country('de')`, `'EL'`, `vat_is_eu_member('EL')` return false though `SplitVAT` accepts those forms. Centralize `NormalizeCountryCode`. | `anofox_vat_country.cpp:15-72`, `anofox_vat.cpp:41,106,115` |
| MED | `<cctype>` on plain char in `NormalizeVAT` (theme T8). | `anofox_vat_country.cpp:95-100` |
| MED | `IsValidSyntax` compiles a `std::regex` per row (theme T3). Precompile in `CountryInfo` during registry init. | `anofox_vat_country.cpp:80-89` |
| LOW | Generic iterators copy every `string_t` to `std::string`; pass `string_view`/`string_t`, consider `UnaryExecutor`. | hpp:30-81 |

### 2.11 Core (extension entry, trace, telemetry, alias helpers)

| Sev | Finding | Where |
|-----|---------|-------|
| HIGH | Telemetry: `_queue` lazily initialized without `_thread_lock`; `_api_key` copied unguarded in capture paths while `SetAPIKey` writes under mutex — races under parallel execution / concurrent SET. Snapshot under one lock, enqueue outside. | `anofox_tabular_extension.cpp:22-60`, `posthog-telemetry/src/telemetry.cpp:132-185` |
| MED | Alias helpers reconstruct functions copying a small field subset — aliases silently lose bind/statistics/pushdown/serialization/error-mode metadata (theme T10). Copy-then-rename. | `anofox_function_alias.hpp:18-74` |
| MED | `extension_load` telemetry fires before any SQL `SET anofox_telemetry_enabled=false` can run — the comment claiming otherwise is wrong; only `DATAZOO_DISABLE_TELEMETRY` works. Read effective setting first or document honestly. | `anofox_tabular_extension.cpp:54-69` |
| LOW | `AnofoxTrace` multi-op writes to `std::cerr` interleave across parallel tasks and drop the level string. Compose line, guard with mutex, include level. | `anofox_trace.cpp:73,81` |

---

## 3. Suggested implementation plan (for later refinement)

### Phase 1 — Correctness & safety (HIGH, do first)
1. **Shared SQL-quoting helpers** + sweep of metric, outlier_tree, pii, diff (T1).
2. **Thread-safety batch:** CurrencyRegistry ctor-init; PII Base58 static array; PII config snapshotting; NER inference mutex (stopgap) + LRU capacity fix; postal `LoadData`/`data_directory` locking; telemetry lock discipline (T2).
3. **Behavioral bugs:** `money_from_cents` division; DBSCAN minPts/border semantics; isolation-forest refit state clearing; diff nullable-PK presence markers; postal/money parent-struct NULL (T6); email SMTP localhost fallback removal; `FD_SET` guards; replace `std::system("tar …")`.

### Phase 2 — API honesty & validation (HIGH/MEDIUM)
4. Implement or unregister: metric DBSCAN placeholders, `vat_format`, `diff_hashdiff` params, NER model option (T7).
5. VAT checksum validation; phonenumber NANPA region-hint handling and national-number parsing.
6. Validation sweep (T5): signed-then-cast, `isfinite`, strict `output_mode`/format/region options, `SetFallible()` on throwing money functions, metric empty-input semantics.

### Phase 3 — Performance & memory (MEDIUM)
7. Precompile regexes (phonenumber, vat); hoist per-row normalization (email); `string_view`-based row lambdas; `UnaryExecutor`/`BinaryExecutor` adoption for simple scalars (T3).
8. Streaming table functions for pii scan/audit, outlier_tree, isolation forest; `UnifiedVectorFormat` input reads; drop duplicate result copies (T4).
9. Algorithm hot-path cleanups: cached split statistics, reused scratch buffers, DBSCAN neighbor-buffer reuse.

### Phase 4 — Structure & hygiene (LOW)
10. Alias helper copy-then-rename + primary-vs-alias regression tests (T10).
11. Delete dead code: diff native scaffold, metric legacy IF SQL (T9).
12. `<cctype>` unsigned-char sweep (T8); RAII wrappers (sockets, c-ares, curl, `FILE*`); JSON escaping helper; trace mutex + level output; settings-scoping/`FunctionStability` audit.

### Cross-cutting test additions
- Quoted/`'`-containing identifiers through every SQL-generating function.
- `f(NULL) IS NULL` for all struct-returning functions.
- Empty tables, all-NULL columns, constant columns for every metric.
- NaN/Inf/negative parameters for all numeric options.
- Concurrency smoke tests (parallel queries + concurrent `SET`) for ner, pii, money, postal.
- DBSCAN border-point and `min_pts` semantics; isolation-forest double-fit; NANPA `CA` parsing; VAT checksum vectors; `money_from_cents` subunit cases (USD, JPY).
