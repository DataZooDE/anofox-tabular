# Contributing

Thank you for helping with anofox-tabular. Bug reports with a small query and sample data are the
fastest way to a fix: [open an issue](https://github.com/DataZooDE/anofox-tabular/issues).

## Pull requests

1. Fork the repository and create a branch.
2. Build and test as described in [docs/dev/building.md](docs/dev/building.md) and
   [docs/dev/testing.md](docs/dev/testing.md).
3. Add or change a SQL test in `test/sql/` for every behavior change.
4. Document new or changed functions and settings in `docs/api/` and
   `docs/reference/configuration.md`. The test `test/sql/docs_coverage.test` fails when a
   registered function or setting is missing from the docs.
5. Update the matching Claude Code skill in `plugins/anofox-tabular/skills/` (signatures,
   pitfalls, examples), so assistants keep writing working SQL.
6. Run `make test` and `make check-docs` (runs every SQL example of the README, the docs and
   the skills), then open the pull request.

## Developer docs

- [Building](docs/dev/building.md)
- [Testing](docs/dev/testing.md)
- [Architecture](docs/dev/architecture.md)
- [Updating DuckDB](docs/dev/updating-duckdb.md)

`CLAUDE.md` and `AGENTS.md` contain further notes on coding style and project conventions.
