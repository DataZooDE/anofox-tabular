## Summary
The money module has a compact implementation and follows DuckDB’s chunked callback shape, but several correctness contracts are under-specified or broken. The largest risks are exactness of monetary arithmetic, incorrect cents conversion, unsynchronized singleton initialization, and registering fallible functions as non-fallible. NULL handling is mostly covered for top-level SQL NULLs, but child-field NULLs inside `STRUCT` values can produce non-NULL money structs with invalid fields. Performance is acceptable for small vectors, but the current loops do avoidable per-row string copies and repeated struct entry lookups.

## Findings

### [HIGH] Currency registry initialization is not thread-safe
Affected files: `src/anofox_money_currency.cpp:7`, `src/anofox_money_currency.cpp:9`, `src/anofox_money_currency.cpp:15`, `src/include/anofox_money_currency.hpp:51`, `src/include/anofox_money_currency.hpp:52`

`GetInstance()` uses a thread-safe function-local static, but then performs a separate unsynchronized `if (!instance.initialized) instance.Initialize()` check. Under DuckDB parallel execution, two threads can enter this block on first use, concurrently mutate `currencies`, and race on the plain `bool initialized`. That is undefined behavior and can corrupt the registry.

Suggested improvement: initialize the registry in the constructor or use `std::call_once`.

```cpp
class CurrencyRegistry {
private:
    CurrencyRegistry() {
        LoadCurrencies();
    }
    case_insensitive_map_t<unique_ptr<CurrencyInfo>> currencies;
};

CurrencyRegistry &CurrencyRegistry::GetInstance() {
    static CurrencyRegistry instance;
    return instance;
}
```

### [HIGH] `money_from_cents` returns cents as units
Affected files: `src/anofox_money.cpp:70`, `src/anofox_money.cpp:101`, `src/anofox_money.cpp:106`, `src/anofox_money.cpp:108`, `src/include/anofox_money_currency.hpp:18`

`anofox_money_from_cents(10050, 'USD')` returns `10050.0 USD`, not `100.50 USD`. The code even fetches `subunit_to_unit` but does not use it. This makes the function name and normal monetary semantics wrong for every currency with subunits, and it also mishandles zero-decimal currencies such as JPY.

Suggested improvement: divide by `currency->subunit_to_unit`, and reject invalid or zero denominators defensively.

```cpp
const auto divisor = currency->subunit_to_unit;
if (divisor <= 0) {
    throw InternalException("Invalid subunit_to_unit for currency: %s", currency_code.c_str());
}
double amount = static_cast<double>(cents_values[cents_idx]) / static_cast<double>(divisor);
```

### [HIGH] Monetary values are stored and computed as `DOUBLE`
Affected files: `src/anofox_money.cpp:19`, `src/anofox_money.cpp:21`, `src/anofox_money.cpp:364`, `src/anofox_money.cpp:378`, `src/anofox_money.cpp:415`, `src/anofox_money.cpp:630`, `src/include/anofox_money.hpp:18`, `src/include/anofox_money.hpp:45`

The module models `amount` as `DOUBLE`, which makes common money operations inexact and allows `NaN`, infinities, and silent overflow to infinities. Values from cents above `2^53` also lose integer precision when converted to double. This is especially risky because the module exposes arithmetic helpers and formatting, so users will reasonably expect stable cent-level behavior.

Suggested improvement: store minor units as `BIGINT` or use DuckDB `DECIMAL`, with explicit overflow checks for add/subtract/multiply. For a decimal representation, make the struct type explicit:

```cpp
children.push_back(make_pair("amount", LogicalType::DECIMAL(18, 2)));
```

If using minor units, expose formatting and amount extraction through controlled conversion rather than storing approximate floating point values.

### [MEDIUM] Fallible scalar functions are registered as `CANNOT_ERROR`
Affected files: `src/anofox_money.cpp:57`, `src/anofox_money.cpp:97`, `src/anofox_money.cpp:166`, `src/anofox_money.cpp:220`, `src/include/anofox_money.hpp:154`, `src/anofox_money.cpp:619`

DuckDB scalar functions default to `FunctionErrors::CANNOT_ERROR`, but this module throws `InvalidInputException` for invalid currencies and currency mismatches. `BoundFunctionExpression::CanThrow()` relies on the function error mode, so the planner can treat these expressions as non-throwing even though they can fail at runtime.

Suggested improvement: call `SetFallible()` on functions that can throw before registration, including aliases via the existing alias helper.

```cpp
money_func.SetFallible();
currency_symbol_func.SetFallible();
money_add_func.SetFallible();
```

### [MEDIUM] Child-field NULLs produce valid parent money structs
Affected files: `src/include/anofox_money.hpp:144`, `src/include/anofox_money.hpp:148`, `src/anofox_money.cpp:343`, `src/anofox_money.cpp:408`

For `STRUCT` results, invalid rows only mark child vectors invalid. The parent result vector remains valid, so a row can become a non-NULL money struct whose `amount` and `currency` fields are NULL. That differs from normal SQL NULL propagation and can leak through `money_amount`, `money_currency`, or comparisons when users pass manually constructed structs with NULL children.

Suggested improvement: define one invariant. If invalid money means SQL NULL, set the parent struct validity too:

```cpp
FlatVector::SetNull(result, i, true);
builder.amount_validity.SetInvalid(i);
builder.currency_validity.SetInvalid(i);
```

If partially NULL money structs are valid by design, then accessors and predicates should document and consistently handle that model.

### [MEDIUM] Currency normalization is inconsistent
Affected files: `src/anofox_money.cpp:54`, `src/anofox_money.cpp:57`, `src/anofox_money.cpp:61`, `src/include/anofox_money.hpp:151`, `src/include/anofox_money.hpp:154`

The registry lookup is case-insensitive, so `anofox_tab_money(100, 'usd')` is accepted, but the original input string is stored. Later operations compare stored strings case-sensitively, so `money_add(money(1, 'usd'), money(1, 'USD'))` throws even though both resolved to the same currency.

Suggested improvement: canonicalize currency codes at construction time by storing `currency->iso_code`, and use canonical codes for all comparisons and formatting.

```cpp
auto currency = registry.GetCurrency(currency_code);
if (!currency) {
    throw InvalidInputException("Invalid currency code: %s", currency_code.c_str());
}
SetMoneyResult(builder, i, amount, currency->iso_code, result);
```

### [LOW] Formatting ignores currency metadata and special floating-point values
Affected files: `src/anofox_money.cpp:227`, `src/anofox_money.cpp:230`, `src/anofox_money.cpp:248`, `src/anofox_money.cpp:253`, `src/include/anofox_money_currency.hpp:18`, `src/include/anofox_money_currency.hpp:21`

`money_format` always prints two decimals, does not use `subunit_to_unit` to determine scale, never inserts `thousands_separator`, and only swaps the first decimal mark character. It will format JPY as `¥1000.00` and can emit strings for `nan`/`inf` values if such amounts enter the struct.

Suggested improvement: derive scale from currency metadata, reject non-finite amounts if `DOUBLE` remains, and centralize formatting in a helper so symbol/code/long styles share validation.

### [LOW] Per-row string copies and repeated vector lookups add avoidable overhead
Affected files: `src/include/anofox_money.hpp:71`, `src/include/anofox_money.hpp:75`, `src/include/anofox_money.hpp:96`, `src/include/anofox_money.hpp:151`, `src/anofox_money.cpp:153`, `src/anofox_money.cpp:169`, `src/anofox_money.cpp:365`

Most loops call `GetString()` per row, copy into `std::string`, and sometimes call `StructVector::GetEntries(result)` inside the row loop or helper. For DuckDB vectors, this is unnecessary overhead, especially for dictionary/constant vectors with repeated currency codes.

Suggested improvement: cache result child vector references outside loops and pass `string_t` or `string_view` where possible. For simple extraction and predicates, consider DuckDB’s `UnaryExecutor`, `BinaryExecutor`, or `GenericExecutor` patterns to preserve constant/dictionary vector optimizations.

## Quick wins
- Make `CurrencyRegistry` initialize in its constructor and remove the mutable `initialized` flag.
- Fix `money_from_cents` to divide by `subunit_to_unit`.
- Call `SetFallible()` on every function that can throw.
- Canonicalize accepted currency codes to `CurrencyInfo::iso_code` before storing them in money structs.
- Add tests for `money('usd') + money('USD')`, invalid currency errors, child-field NULL structs, `NaN`/`Inf`, and large cent values.
- Move repeated `FlatVector::GetData` and `StructVector::GetEntries` calls out of row lambdas and loops.