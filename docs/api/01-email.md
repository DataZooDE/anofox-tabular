# Email Validation

> Check email addresses by syntax, by DNS MX lookup, or by an SMTP handshake.

**Use this page to:**
- Validate email addresses in a column
- Choose between the `regex`, `dns` and `smtp` modes
- Read the failure reason, MX hosts and SMTP transcript of a check

The default mode `regex` runs locally. The modes `dns` and `smtp` use the network: `dns`
looks up the MX records of the domain, and `smtp` also connects to the mail host on port 25.
See [Network and privacy](../reference/network-and-privacy.md).

## Functions

| Function | Canonical name | Kind | Returns |
|----------|----------------|------|---------|
| [`email_is_valid`](#email_is_valid) | `anofox_tab_email_is_valid` | scalar | `BOOLEAN` |
| [`email_validate`](#email_validate) | `anofox_tab_email_validate` | scalar | `STRUCT` with stage, reason, MX hosts, transcript |
| [`email_config`](#email_config) | `anofox_tab_email_config` | table | the current email settings |

## Validation modes

| Mode | What it checks | Network |
|------|----------------|---------|
| `regex` (default) | The address matches `anofox_tab_email_regex_pattern` | none |
| `dns` | `regex`, then the domain has MX records | DNS query |
| `smtp` | `dns`, then an MX host accepts `RCPT TO` for the address. `full` is an alias. | DNS query, TCP connection to port `anofox_tab_email_smtp_port` |

The mode argument is case-insensitive. When you omit it, or pass `NULL`, the function uses the
setting `anofox_tab_email_default_validation`. An unknown mode does not raise an error: the
result is invalid with stage `unsupported`.

---

## `email_is_valid`

Canonical name: `anofox_tab_email_is_valid`. Scalar function.

Returns `true` when the address passes every stage of the chosen mode.

```text
email_is_valid(email VARCHAR [, mode VARCHAR]) -> BOOLEAN
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `email` | `VARCHAR` | required | The address to check |
| `mode` | `VARCHAR` | `anofox_tab_email_default_validation` (`regex`) | `regex`, `dns` or `smtp` |

A `NULL` email returns `NULL`. An empty string returns `false`.

```sql
SELECT address, email_is_valid(address) AS valid
FROM (VALUES ('user@example.com'), ('no-at-sign.example.com'), (''), (NULL)) t(address);
```

```text
┌────────────────────────┬─────────┐
│        address         │  valid  │
│        varchar         │ boolean │
├────────────────────────┼─────────┤
│ user@example.com       │ true    │
│ no-at-sign.example.com │ false   │
│                        │ false   │
│ NULL                   │ NULL    │
└────────────────────────┴─────────┘
```

---

## `email_validate`

Canonical name: `anofox_tab_email_validate`. Scalar function.

Validates like `email_is_valid` and returns the details of the check.

```text
email_validate(email VARCHAR [, mode VARCHAR]) -> STRUCT(
    valid      BOOLEAN,
    stage      VARCHAR,
    reason     VARCHAR,
    mx_hosts   VARCHAR[],
    smtp_debug STRUCT(transcript VARCHAR[])
)
```

| Field | Type | Description |
|-------|------|-------------|
| `valid` | `BOOLEAN` | `true` when every stage of the mode passed |
| `stage` | `VARCHAR` | The last stage that ran: `regex`, `dns`, `smtp`, or `unsupported` for an unknown mode |
| `reason` | `VARCHAR` | `NULL` when valid; otherwise a code from [Reason codes](#reason-codes) |
| `mx_hosts` | `VARCHAR[]` | MX hosts found by the `dns` and `smtp` stages; empty for `regex` |
| `smtp_debug.transcript` | `VARCHAR[]` | The SMTP conversation of the `smtp` stage; empty otherwise |

```sql
SELECT address,
       (email_validate(address)).valid  AS valid,
       (email_validate(address)).stage  AS stage,
       (email_validate(address)).reason AS reason
FROM (VALUES ('user@example.com'), ('not-an-email')) t(address);
```

```text
┌──────────────────┬─────────┬─────────┬────────────────┐
│     address      │  valid  │  stage  │     reason     │
│     varchar      │ boolean │ varchar │    varchar     │
├──────────────────┼─────────┼─────────┼────────────────┤
│ user@example.com │ true    │ regex   │ NULL           │
│ not-an-email     │ false   │ regex   │ invalid_format │
└──────────────────┴─────────┴─────────┴────────────────┘
```

An unknown mode:

```sql
SELECT email_validate('user@example.com', 'bogus') AS result;
```

```text
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                             result                                                             │
│      struct("valid" boolean, stage varchar, reason varchar, mx_hosts varchar[], smtp_debug struct(transcript varchar[]))       │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ {'valid': false, 'stage': unsupported, 'reason': validation_not_implemented, 'mx_hosts': [], 'smtp_debug': {'transcript': []}} │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

A DNS check needs network access, so its result depends on where you run it:

```sql no-run
-- Uses the network: looks up the MX records of example.org
SELECT email_validate('support@example.org', 'dns');
```

### Reason codes

| Stage | Reason | Meaning |
|-------|--------|---------|
| `regex` | `invalid_format` | The address does not match the regex pattern |
| `unsupported` | `validation_not_implemented` | The mode is not `regex`, `dns`, `smtp` or `full` |
| `dns` | `dns_resolution_failed` | No domain could be taken from the address |
| `dns` | `dns_empty_domain` | The domain is empty |
| `dns` | `dns_domain_not_found` | The domain does not exist (NXDOMAIN) |
| `dns` | `dns_no_records` | The domain has neither MX nor address records |
| `dns` | `dns_null_mx` | The domain publishes a Null MX record (RFC 7505): it accepts no mail |
| `dns` | `dns_timeout` | The lookup did not finish within `anofox_tab_email_dns_timeout_ms` × `anofox_tab_email_dns_tries` |
| `dns` | `dns_server_unreachable` | The DNS server refused the connection |
| `dns` | `dns_out_of_memory` | The resolver ran out of memory |
| `dns` | `dns_init_failed` | The resolver library could not be initialized |
| `dns` | `dns_loop_failure`, `dns_pending_incomplete` | The resolver stopped before all queries finished |
| `dns` | `dns_poll_failed_<errno>` | Waiting on the resolver sockets failed |
| `dns` | `dns_error_<status>` | Any other c-ares status code |
| `smtp` | `smtp_no_hosts` | No mail host to try |
| `smtp` | `smtp_dns_reserved_domain` | The mail host is a reserved name such as `localhost` and is not contacted |
| `smtp` | `smtp_dns_no_addresses`, `smtp_dns_timeout`, `smtp_dns_incomplete`, `smtp_dns_init_failed`, `smtp_dns_poll_failed_<errno>`, `smtp_dns_*` | The mail host name could not be resolved to an address |
| `smtp` | `smtp_socket_init_failed` | The socket subsystem is unavailable |
| `smtp` | `smtp_connect_error`, `smtp_connect_timeout` | No TCP connection to the mail host |
| `smtp` | `smtp_greeting_error`, `smtp_greeting_timeout`, `smtp_invalid_greeting` | The server greeting was missing or not `220` |
| `smtp` | `smtp_helo_rejected` | The server rejected `EHLO` |
| `smtp` | `smtp_mail_from_rejected` | The server rejected `MAIL FROM` |
| `smtp` | `smtp_recipient_rejected` | The server rejected `RCPT TO`: the mailbox does not exist |
| `smtp` | `smtp_read_error`, `smtp_read_timeout`, `smtp_write_error`, `smtp_write_timeout` | The connection failed during the conversation |
| `smtp` | `smtp_invalid_response`, `smtp_response_too_large` | The server sent a malformed or oversized reply |
| `smtp` | `smtp_overall_timeout` | The whole SMTP check ran out of time |
| `smtp` | `smtp_verification_failed` | SMTP failed without a more specific reason |

### DNS cache

DNS results are cached per domain for the life of the process, so a column with many addresses
of the same domain does one lookup instead of one per row. The domain key is case-insensitive.
Entries expire after 300 seconds and the cache holds at most 10,000 domains; when it is full,
expired entries are dropped first, then the entry closest to expiry. Successful lookups and
definite negative answers (`dns_domain_not_found`, `dns_no_records`, `dns_null_mx`) are cached.
Timeouts and other transient errors are not, so the next row with that domain tries again. The
cache is shared by `dns` and `smtp` mode and by all connections; `email_config` reports its
limits as `dns_cache_ttl_s` and `dns_cache_max_entries`.

Per-row outcomes (lookups, results, SMTP steps) are traced at `debug` level. Only environment
problems, such as a resolver that cannot be initialized, are traced at `warn`.

---

## `email_config`

Canonical name: `anofox_tab_email_config`. Table function.

Returns the current email settings as key/value rows, plus the two tracing settings.

```text
email_config() -> TABLE(key VARCHAR, value VARCHAR)
```

```sql
SELECT * FROM email_config();
```

```text
┌─────────────────────────┬──────────────────────────────────────────────────────────────────────┐
│           key           │                                value                                 │
│         varchar         │                               varchar                                │
├─────────────────────────┼──────────────────────────────────────────────────────────────────────┤
│ default_validation      │ regex                                                                │
│ regex_pattern           │ ^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*$ │
│ dns_timeout_ms          │ 1000                                                                 │
│ dns_tries               │ 1                                                                    │
│ dns_cache_ttl_s         │ 300                                                                  │
│ dns_cache_max_entries   │ 10000                                                                │
│ smtp_port               │ 25                                                                   │
│ smtp_connect_timeout_ms │ 5000                                                                 │
│ smtp_read_timeout_ms    │ 5000                                                                 │
│ smtp_helo_domain        │ duckdb.local                                                         │
│ smtp_mail_from          │ validator@duckdb.local                                               │
│ trace_enabled           │ true                                                                 │
│ trace_level             │ info                                                                 │
└─────────────────────────┴──────────────────────────────────────────────────────────────────────┘
  13 rows                                                                              2 columns
```

The settings themselves are described in [Configuration](../reference/configuration.md#email).

## See also

- [Configuration: email settings](../reference/configuration.md#email)
- [Network and privacy](../reference/network-and-privacy.md)
- [Conventions](00-conventions.md)
