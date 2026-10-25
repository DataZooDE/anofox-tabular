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
| `reason` | `VARCHAR` | `NULL` when valid; otherwise a code such as `invalid_format`, `dns_no_records`, `dns_timeout`, `smtp_connect_error` or `validation_not_implemented` |
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
│ smtp_port               │ 25                                                                   │
│ smtp_connect_timeout_ms │ 5000                                                                 │
│ smtp_read_timeout_ms    │ 5000                                                                 │
│ smtp_helo_domain        │ duckdb.local                                                         │
│ smtp_mail_from          │ validator@duckdb.local                                               │
│ trace_enabled           │ true                                                                 │
│ trace_level             │ info                                                                 │
└─────────────────────────┴──────────────────────────────────────────────────────────────────────┘
  11 rows                                                                              2 columns
```

The settings themselves are described in [Configuration](../reference/configuration.md#email).

## See also

- [Configuration: email settings](../reference/configuration.md#email)
- [Network and privacy](../reference/network-and-privacy.md)
- [Conventions](00-conventions.md)
