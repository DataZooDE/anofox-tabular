---
description: 'Reusable check suite with stored result history'
tags: [per-skill, data-quality]
max_turns: 8
allowed_tools: [Skill, Read, Glob, Grep, Write]
---

I use DuckDB with the anofox_tabular extension (already loaded). Tables:

- orders(order_id INTEGER, customer_id INTEGER, amount DECIMAL(10,2), status VARCHAR, created_at TIMESTAMP, order_date DATE), one row per order
- orders_old and orders_new: two versions of orders with the same columns
- orders_expected: an empty table with the agreed column layout of orders
- customers(id INTEGER, name VARCHAR, email VARCHAR, phone VARCHAR, country VARCHAR -- ISO 3166 alpha-2 code, vat_id VARCHAR, notes VARCHAR)

Write the SQL as one script to the file answer.sql in the current directory, so that I can run it as-is with the DuckDB CLI, then reply with one short sentence. The tables are not available to you here, so do not try to run anything.

Task: set up a reusable data quality check suite for orders: at least one row, order_id never null and unique, status only in open/shipped/cancelled, amount not negative. Run it and keep the result history in a table.
