// ---------------------------------------------------------------------------
// SQLite wrapper.
//
// When the project is built with the bundled amalgamation (MF_HAS_SQLITE) this
// is a thin, RAII, exception-translating layer over sqlite3. Without it the
// same API is backed by a small in-memory engine supporting the subset of SQL
// this project issues, so the server still boots.
//
// All access is serialised by a mutex: better-sqlite-style, the connection is
// not shared between simultaneous writers.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "mf/util/json.hpp"

struct sqlite3;

namespace mf::db {

	class Database;

	using Cell = json::Value;          // null | number | string
	using Row = std::vector<Cell>;

	// Column metadata returned alongside query results.
	struct ColumnInfo
	{
		std::string name;
		std::string declaredType;
	};

	struct QueryResult
	{
		std::vector<std::string> columns;
		std::vector<Row> rows;
		std::int64_t changes = 0;
		std::int64_t lastInsertRowid = 0;

		std::size_t size() const { return rows.size(); }
		bool empty() const { return rows.empty(); }

		// First column of the first row, coerced.
		std::int64_t scalarInt(std::int64_t fallback = 0) const;
		double scalarDouble(double fallback = 0.0) const;
		std::string scalarString(const std::string &fallback = "") const;

		// Convert the whole result set into a JSON array of objects.
		json::Array toJson() const;

		// Value by column name in row `index`.
		Cell value(std::size_t index, const std::string &column) const;
	};

	// A prepared statement. Bind by position (1-based internally, 0-based here).
	class Statement
	{
	public:
		Statement() = default;
		Statement(Database *db, std::string sql);

		Statement(const Statement &) = delete;
		Statement &operator=(const Statement &) = delete;
		Statement(Statement &&other) noexcept;
		Statement &operator=(Statement &&other) noexcept;
		~Statement();

		Statement &bind(int index, const Cell &value);
		Statement &bindAll(const Row &values);
		Statement &bindNamed(const std::string &name, const Cell &value);

		QueryResult run();
		std::optional<Row> get();
		std::vector<Row> all();

		const std::string &sql() const { return sql_; }
		void reset();
		void finalize();

	private:
		friend class Database;
		void *handle_ = nullptr; // sqlite3_stmt*
		Database *db_ = nullptr;
		std::string sql_;
		int parameterCount_ = 0;
		bool inMemory_ = false;
	};

	struct OpenOptions
	{
		std::string file; // ignored for in-memory driver
		bool inMemory = false;
		std::string pragmas = "journal_mode = WAL; foreign_keys = ON; busy_timeout = 5000";
		bool readonly = false;
	};

	class Database
	{
	public:
		Database();
		~Database();

		Database(const Database &) = delete;
		Database &operator=(const Database &) = delete;

		// Throws mf::Error(ErrorCode::Database) on failure.
		void open(const OpenOptions &options);
		void close();
		bool isOpen() const { return open_; }
		bool isInMemory() const { return inMemory_; }
		const std::string &file() const { return file_; }

		// Execute one or more statements, ignoring any result rows.
		void execute(const std::string &sql);
		// Execute a SQL script split into statements.
		void executeScript(const std::string &sql);

		QueryResult query(const std::string &sql, const Row &params = {});
		std::optional<Row> queryOne(const std::string &sql, const Row &params = {});
		std::int64_t executeUpdate(const std::string &sql, const Row &params = {});

		Statement prepare(const std::string &sql);

		// Transaction helper: BEGIN / COMMIT with rollback on exception.
		template <typename Fn>
		auto transaction(Fn &&fn) -> decltype(fn())
		{
			execute("BEGIN");
			try
			{
				if constexpr (std::is_void_v<decltype(fn())>)
				{
					fn();
					execute("COMMIT");
				}
				else
				{
					auto result = fn();
					execute("COMMIT");
					return result;
				}
			}
			catch (...)
			{
				try
				{
					execute("ROLLBACK");
				}
				catch (...)
				{ /* swallow - original error wins */
				}
				throw;
			}
		}

		std::int64_t lastInsertRowid() const;
		std::int64_t changes() const;
		int errorCode() const;
		std::string lastError() const;

		// Listing helpers used by the schema inspector endpoint.
		std::vector<std::string> tables();
		std::vector<std::string> views();
		std::vector<ColumnInfo> columns(const std::string &table);
		std::int64_t tableRowCount(const std::string &table);

		// Backup (sqlite VACUUM INTO) - no-op for the in-memory driver.
		bool backupTo(const std::string &targetPath);

		// PRAGMA helpers.
		std::string pragma(const std::string &expression);

		// Split a SQL script into individual statements (semicolon aware).
		static std::vector<std::string> splitStatements(const std::string &script);

		std::mutex &mutex() const { return mutex_; }

	private:
		struct MemoryState;
		friend class Statement;

		void bindAll(Statement &statement, const Row &params);
		QueryResult runStatement(Statement &statement);
		void memoryExecute(const std::string &sql, const Row &params, QueryResult *out);

		sqlite3 *handle_ = nullptr;
		void *memory_ = nullptr;
		std::shared_ptr<MemoryState> memoryState_;
		bool open_ = false;
		bool inMemory_ = false;
		std::string file_;
		mutable std::mutex mutex_;
		std::string lastError_;
	};

	// Convenience: build a Database from a resolved configuration tree.
	std::unique_ptr<Database> openFromConfig(const json::Value &config);

} // namespace mf::db
