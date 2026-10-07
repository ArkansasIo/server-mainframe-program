#include "mf/db/migrations.hpp"

#include "mf/core/errors.hpp"
#include "mf/core/logger.hpp"
#include "mf/util/fs.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

#include <algorithm>

namespace mf::db
{

	namespace
	{
		constexpr const char *REVISION_SCHEMA = "001_schema";
		constexpr const char *REVISION_SEED = "002_seed";
		constexpr const char *REVISION_VIEWS = "003_views";
	} // namespace

	Migrator::Migrator(Database &database, std::string sqlDirectory)
		: database_(database), sqlDirectory_(std::move(sqlDirectory))
	{
	}

	std::string Migrator::readSqlFile(const std::string &relativePath) const
	{
		const fsutil::fs::path path = fsutil::fs::path(sqlDirectory_) / relativePath;
		const auto text = fsutil::tryReadFile(path);
		if (!text)
			throw Error(ErrorCode::Io, "SQL file not found: " + path.string());
		return *text;
	}

	void Migrator::ensureMigrationTable()
	{
		database_.execute(
			"CREATE TABLE IF NOT EXISTS schema_migrations ("
			"  revision    TEXT PRIMARY KEY,"
			"  applied_at  TEXT NOT NULL DEFAULT (datetime('now')),"
			"  description TEXT NOT NULL DEFAULT ''"
			")");
	}

	bool Migrator::isRecorded(const std::string &revision)
	{
		try
		{
			const QueryResult result = database_.query(
				"SELECT COUNT(*) FROM schema_migrations WHERE revision = ?", {Cell(revision)});
			return result.scalarInt(0) > 0;
		}
		catch (const Error &)
		{
			return false;
		}
	}

	void Migrator::record(const std::string &revision)
	{
		database_.executeUpdate(
			"INSERT OR REPLACE INTO schema_migrations (revision, applied_at, description) "
			"VALUES (?, datetime('now'), ?)",
			{Cell(revision), Cell(std::string("applied by mf-db"))});
	}

	std::vector<std::string> Migrator::appliedRevisions()
	{
		std::vector<std::string> out;
		ensureMigrationTable();
		const QueryResult result = database_.query(
			"SELECT revision FROM schema_migrations ORDER BY revision");
		for (const auto &row : result.rows)
		{
			if (!row.empty())
				out.push_back(row[0].toString());
		}
		return out;
	}

	bool Migrator::schemaPresent()
	{
		try
		{
			const QueryResult result = database_.query(
				"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'users'");
			return result.scalarInt(0) > 0;
		}
		catch (const Error &)
		{
			return false;
		}
	}

	bool Migrator::isEmpty()
	{
		if (!schemaPresent())
			return true;
		try
		{
			return database_.query("SELECT COUNT(*) FROM users").scalarInt(0) == 0;
		}
		catch (const Error &)
		{
			return true;
		}
	}

	void Migrator::applySchema()
	{
		const std::string script = readSqlFile("schema.sql");
		database_.executeScript(script);
		record(REVISION_SCHEMA);
	}

	void Migrator::applyViews()
	{
		const std::string script = readSqlFile("views.sql");
		database_.executeScript(script);
		record(REVISION_VIEWS);
	}

	void Migrator::applySeed(bool force)
	{
		if (!force && isRecorded(REVISION_SEED) && !isEmpty())
			return;
		const std::string script = readSqlFile("seed.sql");
		database_.executeScript(script);
		record(REVISION_SEED);
	}

	MigrationStatus Migrator::migrate()
	{
		ensureMigrationTable();

		MigrationStatus result;
		result.schemaApplied = database_.query(
											"SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='systems'")
								   .scalarInt(0) > 0;
		result.viewsApplied = database_.query(
										   "SELECT COUNT(*) FROM sqlite_master WHERE type='view' AND name='v_job_summary'")
								  .scalarInt(0) > 0;

		// Always (re)apply the schema: every statement is CREATE ... IF NOT EXISTS
		// so it is idempotent and picks up newly added tables.
		applySchema();

		if (!isRecorded(REVISION_SEED) || isEmpty())
		{
			applySeed(true);
			result.seedApplied = true;
		}

		// Views are dropped and recreated each run so edits take effect.
		applyViews();
		result.viewsApplied = true;
		result.schemaApplied = true;

		result.applied = appliedRevisions();

		try
		{
			result.userCount = database_.query("SELECT COUNT(*) FROM users").scalarInt(0);
			result.datasetCount = database_.query("SELECT COUNT(*) FROM datasets").scalarInt(0);
			result.jobCount = database_.query("SELECT COUNT(*) FROM jobs").scalarInt(0);
		}
		catch (const Error &)
		{
			// counts stay zero when the tables are missing
		}

		const std::vector<std::string> all = {REVISION_SCHEMA, REVISION_SEED, REVISION_VIEWS};
		for (const auto &revision : all)
		{
			if (!isRecorded(revision))
				result.pending.push_back(revision);
		}

		return result;
	}

	MigrationStatus Migrator::status()
	{
		MigrationStatus result;
		ensureMigrationTable();
		result.applied = appliedRevisions();
		result.schemaApplied = database_.query(
											"SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='systems'")
								   .scalarInt(0) > 0;
		result.viewsApplied = database_.query(
										   "SELECT COUNT(*) FROM sqlite_master WHERE type='view' AND name='v_job_summary'")
								  .scalarInt(0) > 0;
		result.seedApplied = isRecorded(REVISION_SEED);

		if (result.schemaApplied)
		{
			try
			{
				result.userCount = database_.query("SELECT COUNT(*) FROM users").scalarInt(0);
				result.datasetCount = database_.query("SELECT COUNT(*) FROM datasets").scalarInt(0);
				result.jobCount = database_.query("SELECT COUNT(*) FROM jobs").scalarInt(0);
			}
			catch (const Error &)
			{ /* tables partially present */
			}
		}

		const std::vector<std::string> all = {REVISION_SCHEMA, REVISION_SEED, REVISION_VIEWS};
		for (const auto &revision : all)
		{
			if (!isRecorded(revision))
				result.pending.push_back(revision);
		}
		return result;
	}

	void Migrator::dropAll()
	{
		const std::vector<std::string> views = database_.views();
		const std::vector<std::string> tables = database_.tables();

		database_.execute("PRAGMA foreign_keys = OFF");
		for (const auto &view : views)
		{
			database_.execute("DROP VIEW IF EXISTS " + view);
		}
		for (const auto &table : tables)
		{
			database_.execute("DROP TABLE IF EXISTS " + table);
		}
		database_.execute("PRAGMA foreign_keys = ON");
	}

	std::string Migrator::integrityCheck()
	{
		try
		{
			return database_.query("PRAGMA integrity_check").scalarString("unknown");
		}
		catch (const Error &ex)
		{
			return std::string("error: ") + ex.what();
		}
	}

	void Migrator::vacuum()
	{
		try
		{
			database_.execute("VACUUM");
		}
		catch (const Error &)
		{
			// VACUUM fails inside a transaction; harmless when it does.
		}
	}

	void Migrator::analyze()
	{
		try
		{
			database_.execute("ANALYZE");
		}
		catch (const Error &)
		{
			// ignore
		}
	}

} // namespace mf::db
