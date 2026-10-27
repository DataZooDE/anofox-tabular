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
| `regex` (default) | The address is at most 254 characters with a local part of at most 64, and matches `anofox_tab_email_regex_pattern` | none |
| `dns` | `regex`, then the domain is not a reserved name and has MX records (or, without MX records, an address record) | DNS query |
| `smtp` | `dns`, then an MX host accepts `RCPT TO` for the address. `full` is an alias. | DNS query, TCP connection to port `anofox_tab_email_smtp_port` |

### Syntax rules of the `regex` stage

The default pattern accepts what mail systems accept in practice:

- **Local part** (before the `@`): letters, digits and ``!#$%&'*+/=?^_`{|}~-``, in dot-separated
  parts. A dot may not start or end the local part, and two dots may not follow each other
  (`.max@web.de`, `max.@web.de` and `max..muster@web.de` are invalid). Plus-addressing such as
  `max+news@web.de` is valid. Quoted local parts (`"max muster"@web.de`) are rejected, although
  RFC 5322 allows them.
- **Domain**: labels of 1 to 63 letters, digits and hyphens, separated by dots. A label may not
  start or end with a hyphen (`max@-web.de` is invalid), and the last label may not be all
  digits (`max@web.123` is invalid). A domain with a single label (`max@localhost`) passes the
  `regex` stage; the `dns` stage rejects reserved names.
- **International domains** must be in their ASCII (punycode) form: `max@xn--mller-kva.de` is
  valid, `max@müller.de` is not. Non-ASCII local parts are rejected as well.
- **Length**: at most 254 characters in total and 64 in the local part (RFC 5321). A longer
  address fails with reason `invalid_length`. The length check runs before the pattern, also
  for a custom `anofox_tab_email_regex_pattern`.

Whitespace around the address (spaces, tabs, line breaks) is ignored in every mode, as
`vat_is_valid` ignores it around a VAT number: `' max@web.de '` is valid, and the `dns` and
`smtp` stages use the trimmed address. Whitespace inside the address (`'max muster@web.de'`)
still makes it invalid. The functions return no cleaned address; store `trim(email)` (and
`lower(...)` if you want one spelling) yourself.

```sql
SELECT address, email_is_valid(address) AS valid, (email_validate(address)).reason AS reason
FROM (VALUES ('max.mustermann@web.de'), (' Max.Mustermann@Web.de '), ('max+news@web.de'), ('max..muster@web.de'),
             ('max@-web.de'), ('max@web.123'), ('"max muster"@web.de'),
             (repeat('a', 65) || '@web.de')) t(address);
```

```text
┌──────────────────────────────────────────────────────────────────────────┬─────────┬────────────────┐
│                                 address                                  │  valid  │     reason     │
│                                 varchar                                  │ boolean │    varchar     │
├──────────────────────────────────────────────────────────────────────────┼─────────┼────────────────┤
│ max.mustermann@web.de                                                    │ true    │ NULL           │
│  Max.Mustermann@Web.de                                                   │ true    │ NULL           │
│ max+news@web.de                                                          │ true    │ NULL           │
│ max..muster@web.de                                                       │ false   │ invalid_format │
│ max@-web.de                                                              │ false   │ invalid_format │
│ max@web.123                                                              │ false   │ invalid_format │
│ "max muster"@web.de                                                      │ false   │ invalid_format │
│ aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa@web.de │ false   │ invalid_length │
└──────────────────────────────────────────────────────────────────────────┴─────────┴────────────────┘
```

The mode argument is case-insensitive and surrounding spaces are ignored; `full` is an alias of
`smtp`. When you omit it, or pass `NULL`, the function uses the setting
`anofox_tab_email_default_validation`. An unknown mode raises an error, whatever the address, so
a typo such as `'mx'` cannot mark every address invalid without a hint.

---

## `email_is_valid`

Canonical name: `anofox_tab_email_is_valid`. Scalar function.

Returns `true` when the address passes every stage of the chosen mode, `false` when a stage
gives a definite answer that it does not, and `NULL` when a `dns` or `smtp` check could not
finish (a timeout, an unreachable server, a temporary SMTP refusal). See
[Definite and unknown outcomes](#definite-and-unknown-outcomes).

```text
email_is_valid(email VARCHAR [, mode VARCHAR]) -> BOOLEAN
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `email` | `VARCHAR` | required | The address to check |
| `mode` | `VARCHAR` | `anofox_tab_email_default_validation` (`regex`) | `regex`, `dns` or `smtp` |

A `NULL` email returns `NULL`. An empty string returns `false`. The `regex` mode never returns
`NULL` for a non-`NULL` address.

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
| `valid` | `BOOLEAN` | `true` when every stage of the mode passed, `false` for a definite failure, `NULL` when the check could not finish (see [Definite and unknown outcomes](#definite-and-unknown-outcomes)) |
| `stage` | `VARCHAR` | The last stage that ran: `regex`, `dns` or `smtp` |
| `reason` | `VARCHAR` | `NULL` when valid; otherwise a code from [Reason codes](#reason-codes), also when `valid` is `NULL` |
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

An unknown mode raises an error:

```sql
SELECT email_validate('user@example.com', 'mx') AS result;
```

```text
Invalid Input Error: Unknown email validation mode: mx (expected regex, dns, smtp)
```

### The `dns` stage

The `dns` stage looks up the MX records of the domain. When the domain has none, it falls back
to the domain's address records (A/AAAA), as RFC 5321 prescribes for mail delivery; `mx_hosts`
then lists those IP addresses instead of host names. A Null MX record (RFC 7505) means the domain
accepts no mail and fails with `dns_null_mx`.

Reserved names are rejected without a lookup, with reason `dns_reserved_domain`: `localhost`
and any name ending in `.localhost`, and the reserved top-level domains `example`, `invalid`,
`test` (RFC 2606) and `local` (multicast DNS), including names that end in them. Second-level
names such as `example.com` are looked up normally. The `smtp` mode runs the `dns` stage first,
so these addresses never reach SMTP.

```sql
SELECT (r).valid, (r).stage, (r).reason, (r).mx_hosts
FROM (SELECT email_validate('max@localhost', 'dns') AS r);
```

```text
┌─────────────┬───────────┬─────────────────────┬──────────────┐
│ (r)."valid" │ (r).stage │     (r).reason      │ (r).mx_hosts │
│   boolean   │  varchar  │       varchar       │  varchar[]   │
├─────────────┼───────────┼─────────────────────┼──────────────┤
│ false       │ dns       │ dns_reserved_domain │ []           │
└─────────────┴───────────┴─────────────────────┴──────────────┘
```

A DNS check of a real domain needs network access, so its result depends on where you run it:

```sql no-run
-- Uses the network: looks up the MX records of example.org
SELECT email_validate('support@example.org', 'dns');
```

### Reason codes

The `valid` column shows what `email_validate(...).valid` and `email_is_valid` return for each
reason.

| Stage | Reason | `valid` | Meaning |
|-------|--------|---------|---------|
| `regex` | `invalid_format` | `false` | The address does not match the regex pattern |
| `regex` | `invalid_length` | `false` | The address is longer than 254 characters, or its local part longer than 64 |
| `dns` | `dns_resolution_failed` | `false` | No domain could be taken from the address |
| `dns` | `dns_empty_domain` | `false` | The domain is empty |
| `dns` | `dns_reserved_domain` | `false` | The domain is `localhost`, ends in `.localhost`, or is in the reserved top-level domain `example`, `invalid`, `test` or `local`; it is not looked up |
| `dns` | `dns_domain_not_found` | `false` | The domain does not exist (NXDOMAIN) |
| `dns` | `dns_no_records` | `false` | The domain has neither MX nor address records |
| `dns` | `dns_null_mx` | `false` | The domain publishes a Null MX record (RFC 7505): it accepts no mail |
| `dns` | `dns_timeout` | `NULL` | The lookup did not finish within `anofox_tab_email_dns_timeout_ms` × `anofox_tab_email_dns_tries` |
| `dns` | `dns_server_unreachable` | `NULL` | The DNS server refused the connection |
| `dns` | `dns_out_of_memory` | `NULL` | The resolver ran out of memory |
| `dns` | `dns_init_failed` | `NULL` | The resolver library could not be initialized |
| `dns` | `dns_loop_failure`, `dns_pending_incomplete` | `NULL` | The resolver stopped before all queries finished |
| `dns` | `dns_poll_failed_<errno>` | `NULL` | Waiting on the resolver sockets failed |
| `dns` | `dns_error_<status>` | `NULL` | Any other c-ares status code, such as a server failure (SERVFAIL) |
| `smtp` | `smtp_no_hosts` | `false` | No mail host to try |
| `smtp` | `smtp_dns_reserved_domain` | `false` | The mail host is a reserved name such as `localhost` and is not contacted |
| `smtp` | `smtp_dns_domain_not_found`, `smtp_dns_no_records`, `smtp_dns_no_addresses` | `false` | The mail host name does not exist or has no address |
| `smtp` | `smtp_dns_timeout`, `smtp_dns_incomplete`, `smtp_dns_init_failed`, `smtp_dns_poll_failed_<errno>`, other `smtp_dns_*` | `NULL` | The mail host name could not be resolved in time |
| `smtp` | `smtp_socket_init_failed` | `NULL` | The socket subsystem is unavailable |
| `smtp` | `smtp_connect_error`, `smtp_connect_timeout` | `NULL` | No TCP connection to the mail host (often a blocked port 25) |
| `smtp` | `smtp_greeting_error`, `smtp_greeting_timeout`, `smtp_invalid_greeting` | `NULL` | The server greeting was missing or not `220` |
| `smtp` | `smtp_helo_rejected` | `NULL` | The server rejected `EHLO` |
| `smtp` | `smtp_mail_from_rejected` | `NULL` | The server rejected `MAIL FROM`: it refuses the sender, which says nothing about the recipient |
| `smtp` | `smtp_recipient_rejected` | `false` | The server rejected `RCPT TO` permanently (5xx): the mailbox does not exist. The other mail hosts of the domain are not asked |
| `smtp` | `smtp_recipient_deferred` | `NULL` | The server refused `RCPT TO` temporarily (4xx), e.g. greylisting |
| `smtp` | `smtp_read_error`, `smtp_read_timeout`, `smtp_write_error`, `smtp_write_timeout` | `NULL` | The connection failed during the conversation |
| `smtp` | `smtp_invalid_response`, `smtp_response_too_large` | `NULL` | The server sent a malformed or oversized reply |
| `smtp` | `smtp_overall_timeout` | `NULL` | The whole SMTP check ran out of time |
| `smtp` | `smtp_verification_failed` | `NULL` | SMTP failed without a more specific reason |

### Definite and unknown outcomes

A `dns` or `smtp` check can end without an answer: the DNS server does not reply in time, port
25 is blocked, or the mail server asks to try again later. Such an outcome says nothing about the
address, so `valid` is `NULL` and `reason` says what happened. `false` is reserved for definite
answers: bad syntax, a reserved or non-existent domain, a domain without mail hosts, or a mailbox
that the mail server rejects. A reason code not in the table above is treated as unknown.

Count the outcomes separately, and re-check the unknown ones later:

```sql no-run
-- Uses the network
SELECT count(*) FILTER (WHERE v)          AS valid,
       count(*) FILTER (WHERE NOT v)      AS invalid,
       count(*) FILTER (WHERE v IS NULL)  AS unknown
FROM (SELECT email_is_valid(email, 'dns') AS v FROM customers);
```

In a `WHERE` clause, `email_is_valid(email, 'dns')` keeps only the confirmed addresses, and
`NOT email_is_valid(email, 'dns')` only the definitely invalid ones; the unknown rows are in
neither. A 1 ms DNS budget forces the unknown outcome for a domain the resolver has not cached:

```sql no-run
SET anofox_tab_email_dns_timeout_ms = 1;
SELECT (r).valid, (r).reason FROM (SELECT email_validate('lena@some-uncached-domain.com', 'dns') AS r);
-- valid = NULL, reason = dns_timeout
```

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
┌─────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│           key           │                                                                                        value                                                                                         │
│         varchar         │                                                                                       varchar                                                                                        │
├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ default_validation      │ regex                                                                                                                                                                                │
│ regex_pattern           │ ^[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]+(?:\.[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]+)*@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)*(?![0-9]+$)[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$ │
│ dns_timeout_ms          │ 1000                                                                                                                                                                                 │
│ dns_tries               │ 1                                                                                                                                                                                    │
│ dns_cache_ttl_s         │ 300                                                                                                                                                                                  │
│ dns_cache_max_entries   │ 10000                                                                                                                                                                                │
│ smtp_port               │ 25                                                                                                                                                                                   │
│ smtp_connect_timeout_ms │ 5000                                                                                                                                                                                 │
│ smtp_read_timeout_ms    │ 5000                                                                                                                                                                                 │
│ smtp_helo_domain        │ duckdb.local                                                                                                                                                                         │
│ smtp_mail_from          │ validator@duckdb.local                                                                                                                                                               │
│ trace_enabled           │ true                                                                                                                                                                                 │
│ trace_level             │ info                                                                                                                                                                                 │
└─────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
  13 rows                                                                                                                                                                                              2 columns
```

The settings themselves are described in [Configuration](../reference/configuration.md#email).

## See also

- [Configuration: email settings](../reference/configuration.md#email)
- [Network and privacy](../reference/network-and-privacy.md)
- [Conventions](00-conventions.md)
