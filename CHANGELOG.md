# Changelog

## [v0.3]

Improvements in Session Tracing:
- Fixed PL/pgSQL function tracing – now works correctly.
- Added PID at the start of the file for better trace identification.
- Added query start time for precise timing analysis.
- Improved format for nested queries (better readability and structure).
- Updated documentation and JSON Schema for session trace output format.

Other Changes:
- Fixed stat_hist_uprobe SQL function (resolved known issues).
- Added test suite for the extension:
    Tests are written in Python using the testgres library. 
    Prerequisites:
    1. Ensure PG_CONFIG environment variable points to the pg_config executable.
    2. Run make python_tests to execute the tests.
- Added FreeBSD support.
- Upgraded Frida-Gum to version 17.1.5.