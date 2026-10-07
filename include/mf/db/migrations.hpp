// ---------------------------------------------------------------------------
// Schema management: applies sql/schema.sql, sql/seed.sql and sql/views.sql and
// tracks which revisions have been applied in the schema_migrations table.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include "mf/db/sqlite.hpp"

namespace mf::db
{

	struct MigrationStatus
	{
		bool schemaApplied = false;
		bool seedApplied = false;
		bool viewsApplied = false;
		std::vector<std::string> applied;
		std::vector<std::string> pending;
		std::int64_t userCount = 0;
		std::int64_t datasetCount = 0;
		std::int64_t jobCount = 0;
	};

	class Migrator
	{
	public:
		Migrator(Database &database, std::string sqlDirectory);

		// Create the schema_migrations bookkeeping table.
		void ensureMigrationTable();

		// Apply everything that is pending and record it.
		MigrationStatus migrate();

		// Apply only the schema, or only the seed/views.
		void applySchema();
		void applyViews();
		void applySeed(bool force = false);

		// Does the database already contain the core tables?
		bool schemaPresent();
		bool isEmpty();

		MigrationStatus status();

		// Which revision names have been recorded.
		std::vector<std::string> appliedRevisions();

		// Drop every user table (used by `mf-db reset`).
		void dropAll();

		// Integrity helpers used by the maintenance CLI.
		std::string integrityCheck();
		void vacuum();
		void analyze();

		const std::string &sqlDirectory() const { return sqlDirectory_; }
		void setSqlDirectory(std::string directory) { sqlDirectory_ = std::move(directory); }

	private:
		void record(const std::string &revision);
		bool isRecorded(const std::string &revision);
		std::string readSqlFile(const std::string &relativePath) const;

		Database &database_;
		std::string sqlDirectory_;
	};

} // namespace mf::db
