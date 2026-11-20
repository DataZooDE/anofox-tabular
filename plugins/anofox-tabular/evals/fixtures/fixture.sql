-- Sample tables for the plugin eval suite (scripts/run_plugin_evals.py runs this file
-- before each answer.sql the agent wrote). The tables match the description in every
-- case's prompt.md. Dates are relative to current_date, so a check of "yesterday's"
-- volume always finds a normal day: five orders on each of the last 60 days.
CREATE TABLE orders AS
SELECT i::INTEGER AS order_id,
       (100 + i % 7)::INTEGER AS customer_id,
       (CASE WHEN i = 42 THEN 9999 ELSE 20 + (i % 13) * 1.5 END)::DECIMAL(10,2) AS amount,
       (['open', 'shipped', 'cancelled'])[1 + i % 3] AS status,
       (current_date - 1 - (i % 60)::INTEGER) + INTERVAL (i % 24) HOUR AS created_at,
       (current_date - 1 - (i % 60)::INTEGER)::DATE AS order_date
FROM range(1, 301) t(i);

CREATE TABLE orders_old AS SELECT * FROM orders WHERE order_id <= 290;

CREATE TABLE orders_new AS
SELECT order_id, customer_id,
       CASE WHEN order_id % 50 = 0 THEN amount + 1 ELSE amount END AS amount,
       CASE WHEN order_id = 7 THEN 'cancelled' ELSE status END AS status,
       created_at, order_date
FROM orders WHERE order_id <> 3;

CREATE TABLE orders_expected AS SELECT * FROM orders LIMIT 0;

CREATE TABLE customers AS SELECT * FROM (VALUES
  (1, 'Anna Schmidt', 'anna@example.de', '030 12345678', 'DE', 'DE123456789', 'call anna@example.de or +49 171 1234567'),
  (2, 'John Doe', 'john@@bad', '(650) 253-0000', 'US', NULL, 'no notes'),
  (3, 'Lea Gruber', 'lea@example.at', '01 5321234', 'AT', 'ATU12345678', 'mobile 0664 1234567')
) t(id, name, email, phone, country, vat_id, notes);
