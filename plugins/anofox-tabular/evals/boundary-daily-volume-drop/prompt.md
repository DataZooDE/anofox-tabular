---
description: 'A daily count against its own history is data quality, not row-level anomaly detection'
tags: [boundary, data-quality]
max_turns: 8
allowed_tools: [Skill, Read, Glob, Grep, Write]
---

I use DuckDB with the anofox_tabular extension (already loaded). Tables:

- orders(order_id INTEGER, customer_id INTEGER, amount DECIMAL(10,2), status VARCHAR, created_at TIMESTAMP, order_date DATE), one row per order
- orders_old and orders_new: two versions of orders with the same columns
- orders_expected: an empty table with the agreed column layout of orders
- customers(id INTEGER, name VARCHAR, email VARCHAR, phone VARCHAR, country VARCHAR -- ISO 3166 alpha-2 code, vat_id VARCHAR, notes VARCHAR)

Write the SQL as one script to the file answer.sql in the current directory, so that I can run it as-is with the DuckDB CLI, then reply with one short sentence. The tables are not available to you here, so do not try to run anything.

Task: my nightly job should fail when yesterday's number of orders is unusual compared to the previous weeks.
