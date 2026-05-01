-- smoke_test.sql
-- Self-contained smoke test for the anofox_tabular extension.
-- The LOAD statement is prepended by smoke_test.sh before this file is read.
-- Each query either prints 'OK: ...' or calls error() to abort with a non-zero exit.
-- If the extension is not loaded, the first function call fails with "function not found".

-- 1. Verify the extension registered its functions.
-- COUNT is computed in a subquery to avoid DuckDB's aggregate-in-CASE evaluation quirk.
SELECT CASE
    WHEN cnt >= 5 THEN 'OK: extension functions registered (' || cnt::VARCHAR || ' functions)'
    ELSE error('FAIL: expected >= 5 anofox functions, got ' || cnt::VARCHAR)
END
FROM (
    SELECT COUNT(*) AS cnt
    FROM duckdb_functions()
    WHERE function_name LIKE 'anofox_tab_%'
) t;

-- 2. Email validation: known-good address
SELECT CASE
    WHEN email_is_valid('user@example.com') = true THEN 'OK: email_is_valid true'
    ELSE (SELECT error('FAIL: email_is_valid(''user@example.com'') should be true'))
END;

-- 3. Email validation: known-bad address
SELECT CASE
    WHEN email_is_valid('not-an-email') = false THEN 'OK: email_is_valid false'
    ELSE (SELECT error('FAIL: email_is_valid(''not-an-email'') should be false'))
END;

-- 4. Phone formatting: US number to E164
SELECT CASE
    WHEN phonenumber_format('6502530000', 'US', 'E164') = '+16502530000' THEN 'OK: phonenumber_format E164'
    ELSE (SELECT error('FAIL: phonenumber_format returned: ' ||
          COALESCE(phonenumber_format('6502530000', 'US', 'E164'), 'NULL')))
END;

-- 5. Phone parsing: valid international number
SELECT CASE
    WHEN phonenumber_parse('+1 650-253-0000', NULL).valid = true THEN 'OK: phonenumber_parse valid'
    ELSE (SELECT error('FAIL: phonenumber_parse(''+1 650-253-0000'', NULL).valid should be true'))
END;
