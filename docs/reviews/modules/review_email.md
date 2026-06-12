## Summary
The email module has a reasonable split between scalar registration, DNS lookup, and SMTP probing, but several network-facing paths need tightening before this is safe under DuckDB workloads. The biggest correctness issue is that SMTP validation continues with fallback hosts, including loopback, after DNS failure, which can turn invalid domains into valid addresses depending on the local environment. The DNS/SMTP event loops also use `select()`/`FD_SET` without guarding against high file descriptors, which is a real undefined-behavior risk in long-running processes. Performance-wise, the scalar functions are row-oriented and allocation-heavy, with repeated vector normalization and blocking network calls per row.

## Findings

### [HIGH] SMTP validation can succeed after DNS failure by probing fallback hosts
Affected: `src/anofox_email.cpp:317`, `src/anofox_email.cpp:320`, `src/anofox_email.cpp:321`, `src/anofox_email.cpp:86`, `src/anofox_email.cpp:87`, `src/anofox_email.cpp:88`

When DNS lookup fails, SMTP mode builds fallback hosts from the original domain plus `localhost` and `127.0.0.1`, then continues SMTP verification. This means `email_validate('user@nonexistent.invalid', 'smtp')` can still become valid if a local SMTP server accepts the recipient, even though the recipient domain did not resolve. That is a correctness bug and also creates surprising local network probing behavior.

Suggested improvement: fail SMTP validation when DNS fails, or only use fallback hosts behind an explicit test/debug option.

```cpp
if (!dns_lookup.success) {
    dns_stage.valid = false;
    dns_stage.reason = dns_lookup.reason.empty() ? EMAIL_REASON_DNS_FAIL : dns_lookup.reason;
    return dns_stage;
}
```

### [HIGH] `select()` use can corrupt memory for high-numbered sockets
Affected: `src/anofox_email_dns.cpp:188`, `src/anofox_email_dns.cpp:192`, `src/anofox_email_dns.cpp:195`, `src/anofox_email_smtp.cpp:155`, `src/anofox_email_smtp.cpp:162`, `src/anofox_email_smtp.cpp:165`, `src/anofox_email_smtp.cpp:350`, `src/anofox_email_smtp.cpp:354`, `src/anofox_email_smtp.cpp:357`

The DNS and SMTP loops put arbitrary socket descriptors into `fd_set` without checking `sock < FD_SETSIZE`. On Unix, `FD_SET` with a descriptor greater than or equal to `FD_SETSIZE` is undefined behavior and commonly writes past the bitmap. DuckDB may run inside processes with many open files, so this can surface nondeterministically.

Suggested improvement: replace `select()` with `poll()`/`WSAPoll()` or c-ares’ socket/event helpers that do not depend on `FD_SETSIZE`. If retaining `select()` temporarily, reject oversized descriptors before `FD_SET`.

```cpp
#ifndef _WIN32
if (sock < 0 || sock >= FD_SETSIZE) {
    error_reason = "dns_socket_fd_too_large";
    return false;
}
#endif
```

### [MEDIUM] Null MX records are treated as deliverable MX hosts
Affected: `src/anofox_email_dns.cpp:121`, `src/anofox_email_dns.cpp:122`, `src/anofox_email_dns.cpp:124`, `src/anofox_email_dns.cpp:426`, `src/anofox_email_dns.cpp:434`

Domains can publish a Null MX record with exchange `"."` to state that they accept no email. The current parser stores any MX exchange string and later marks DNS validation successful whenever the MX record list is non-empty. As a result, DNS mode can report a Null MX domain as valid, and SMTP mode may try to resolve/connect to `"."`.

Suggested improvement: detect `"."` during MX parsing and return a specific failure reason such as `dns_null_mx`.

```cpp
if (exchange && std::string(exchange) == ".") {
    state.status = ARES_ENODATA;
    state.error = "dns_null_mx";
    continue;
}
```

### [MEDIUM] SMTP reads have no response size or line-count limits
Affected: `src/anofox_email_smtp.cpp:607`, `src/anofox_email_smtp.cpp:611`, `src/anofox_email_smtp.cpp:646`, `src/anofox_email_smtp.cpp:670`

`ReadLine` keeps appending to `buffer` until it sees `\r\n`, with no maximum line length or total response size. A peer can continuously send data without line terminators and grow memory until the process is pressured; because each successful read restarts the wait cycle, this can also run much longer than expected. Multi-line SMTP responses are similarly unbounded.

Suggested improvement: enforce protocol limits for line length, total response bytes, and multi-line count.

```cpp
constexpr size_t MAX_SMTP_LINE = 8192;
constexpr size_t MAX_SMTP_RESPONSE = 65536;
if (buffer.size() > MAX_SMTP_RESPONSE) {
    result.reason = "smtp_response_too_large";
    return false;
}
```

### [MEDIUM] SMTP total timeout is not actually enforced inside a host attempt
Affected: `src/anofox_email_smtp.cpp:927`, `src/anofox_email_smtp.cpp:928`, `src/anofox_email_smtp.cpp:931`, `src/anofox_email_smtp.cpp:935`

`SmtpClient::Verify` computes an overall deadline, but it only checks that deadline before each host. `AttemptHost` can then spend one connect timeout plus several read/write timeouts per endpoint, and a host with many resolved endpoints can exceed `MAX_TOTAL_TIMEOUT_MS` by a wide margin. This makes query latency hard to bound, especially under parallel execution.

Suggested improvement: pass an absolute deadline into `AttemptHost`, `ConnectSocket`, `ReadResponse`, and `SendCommand`, and cap each wait by the remaining budget.

```cpp
auto remaining = std::chrono::duration_cast<milliseconds>(deadline - Clock::now());
if (remaining <= milliseconds::zero()) {
    result.reason = "smtp_overall_timeout";
    return false;
}
```

### [MEDIUM] Scalar execution bypasses DuckDB vectorized access patterns
Affected: `src/anofox_email.cpp:420`, `src/anofox_email.cpp:421`, `src/anofox_email.cpp:432`, `src/anofox_email.cpp:469`, `src/anofox_email.cpp:470`, `src/anofox_email.cpp:481`

Both scalar functions call `args.GetValue(0, row)` per row, convert values to `std::string`, and call `ExtractValidationMode`, which itself may call `ToUnifiedFormat` for every row. This adds avoidable allocations and repeated vector normalization, and it is especially expensive for regex-only validation where DuckDB can process flat/constant vectors much more cheaply.

Suggested improvement: normalize input vectors once per chunk, use `UnifiedVectorFormat`, and cache constant/default mode outside the row loop. For regex-only paths, consider `UnaryExecutor`/`BinaryExecutor`-style execution.

### [LOW] Global email options do not respect connection/database scoping
Affected: `src/anofox_email.cpp:92`, `src/anofox_email.cpp:94`, `src/anofox_email.cpp:99`, `src/anofox_email.cpp:674`, `src/anofox_email.cpp:676`

The extension registers DuckDB options, but the actual runtime state is a process-global singleton. The mutex prevents data races, but one connection changing `anofox_tab_email_regex_pattern` or SMTP settings affects other concurrent connections and databases using the same extension instance. That can produce surprising query results under parallel or embedded use.

Suggested improvement: store effective options in DuckDB configuration/client context where possible, or bind a snapshot of the relevant settings into `FunctionData` so a query has stable semantics.

### [LOW] DNS retry option can overflow/truncate internally
Affected: `src/anofox_email.cpp:568`, `src/anofox_email.cpp:572`, `src/anofox_email.cpp:573`, `src/anofox_email_dns.cpp:363`

`SetDnsTriesOption` allows values up to `uint32_t::max()`, returns the value as `INTEGER`, and later casts it to `int` for c-ares. Values above `INT_MAX` can wrap or become negative in those casts. The registered DuckDB option is `INTEGER`, so the practical input path may already constrain this, but the validation and internal types disagree.

Suggested improvement: cap tries to a small documented maximum that fits `int`, such as `1..10`, and use the same bound everywhere.

## Quick wins
- Remove `localhost` and `127.0.0.1` from SMTP fallback behavior.
- Add explicit Null MX handling in DNS parsing.
- Replace or guard all `FD_SET` calls.
- Hoist `ToUnifiedFormat` calls out of per-row loops.
- Add maximum SMTP line/response sizes.
- Use RAII wrappers for sockets and c-ares channels to avoid leaks on future exception paths.
- Mark DNS/SMTP scalar overloads with a less aggressive stability classification than `CONSISTENT`.