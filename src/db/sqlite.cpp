#include "mf/db/sqlite.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/fs.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#if defined(MF_HAS_SQLITE)
#include "sqlite3.h"
#endif

namespace mf::db
{

	namespace
	{

		// SQL used by the fallback in-memory engine, kept in one place so the
		// regex-style matching stays readable.
		bool isSelectLike(const std::string &sql)
		{
			const std::string head = str::lower(sql);
			return str::startsWith(head, "select") || str::startsWith(head, "with") || str::startsWith(head, "pragma") || str::startsWith(head, "explain");
		}

		std::string trimTrailingSemicolon(std::string sql)
		{
			while (!sql.empty() && (sql.back() == ';' || std::isspace(static_cast<unsigned char>(sql.back()))))
			{
				sql.pop_back();
			}
			return sql;
		}

		Cell cellFromText(const std::string &text)
		{
			// SQLite is dynamically typed; the wrapper keeps numbers as numbers
			// when they round-trip cleanly so JSON output stays friendly.
			if (text.empty())
				return Cell(std::string());
			return Cell(text);
		}

	} // namespace

	// -----------------------------------------------------------------------
	// QueryResult
	// -----------------------------------------------------------------------
	std::int64_t QueryResult::scalarInt(std::int64_t fallback) const
	{
		if (rows.empty() || rows[0].empty())
			return fallback;
		const Cell &value = rows[0][0];
		if (value.isNumber())
			return value.asInt();
		if (const auto parsed = str::toInt(value.toString()))
			return *parsed;
		return fallback;
	}

	double QueryResult::scalarDouble(double fallback) const
	{
		if (rows.empty() || rows[0].empty())
			return fallback;
		const Cell &value = rows[0][0];
		if (value.isNumber())
			return value.asNumber();
		if (const auto parsed = str::toDouble(value.toString()))
			return *parsed;
		return fallback;
	}

	std::string QueryResult::scalarString(const std::string &fallback) const
	{
		if (rows.empty() || rows[0].empty())
			return fallback;
		const Cell &value = rows[0][0];
		if (value.isNull())
			return fallback;
		if (value.isString())
			return value.asString();
		return value.dumpCompact();
	}

	json::Array QueryResult::toJson() const
	{
		json::Array out;
		out.reserve(rows.size());
		for (const auto &row : rows)
		{
			json::Value object = json::Value::object();
			for (std::size_t i = 0; i < columns.size() && i < row.size(); ++i)
			{
				object.set(columns[i], row[i]);
			}
			out.push_back(std::move(object));
		}
		return out;
	}

	Cell QueryResult::value(std::size_t index, const std::string &column) const
	{
		if (index >= rows.size())
			return Cell();
		const auto it = std::find(columns.begin(), columns.end(), column);
		if (it == columns.end())
			return Cell();
		const std::size_t position = static_cast<std::size_t>(std::distance(columns.begin(), it));
		if (position >= rows[index].size())
			return Cell();
		return rows[index][position];
	}

	// -----------------------------------------------------------------------
	// Memory engine
	// -----------------------------------------------------------------------
	struct Database::MemoryState
	{
		std::map<std::string, std::vector<Row>> tables;
		std::map<std::string, std::vector<std::string>> columns;
		std::map<std::string, std::int64_t> sequences;

		std::vector<Row> &table(const std::string &name) { return tables[str::lower(name)]; }
	};

	// -----------------------------------------------------------------------
	// Statement
	// -----------------------------------------------------------------------
	Statement::Statement(Database *db, std::string sql) : db_(db), sql_(std::move(sql))
	{
		if (!db_ || !db_->isOpen())
			throwDatabase("cannot prepare statement: database is closed");
		inMemory_ = db_->isInMemory();

#if defined(MF_HAS_SQLITE)
		if (!inMemory_)
		{
			sqlite3_stmt *handle = nullptr;
			const int rc = sqlite3_prepare_v2(db_->handle_, sql_.c_str(),
											  static_cast<int>(sql_.size()), &handle, nullptr);
			if (rc != SQLITE_OK)
			{
				throwDatabase(std::string("prepare failed: ") + sqlite3_errmsg(db_->handle_) + " [" + sql_ + "]");
			}
			handle_ = handle;
			parameterCount_ = sqlite3_bind_parameter_count(handle);
		}
#endif
		if (inMemory_)
		{
			// ":name" and "?" placeholders both count.
			int count = 0;
			for (std::size_t i = 0; i < sql_.size(); ++i)
			{
				if (sql_[i] == '?')
					++count;
			}
			if (count == 0)
			{
				// count named parameters as a fallback
				std::size_t pos = 0;
				while ((pos = sql_.find(':', pos)) != std::string::npos)
				{
					++count;
					++pos;
				}
			}
			parameterCount_ = count;
		}
	}

	Statement::Statement(Statement &&other) noexcept
		: handle_(other.handle_), db_(other.db_), sql_(std::move(other.sql_)),
		  parameterCount_(other.parameterCount_), inMemory_(other.inMemory_)
	{
		other.handle_ = nullptr;
		other.db_ = nullptr;
	}

	Statement &Statement::operator=(Statement &&other) noexcept
	{
		if (this != &other)
		{
			finalize();
			handle_ = other.handle_;
			db_ = other.db_;
			sql_ = std::move(other.sql_);
			parameterCount_ = other.parameterCount_;
			inMemory_ = other.inMemory_;
			other.handle_ = nullptr;
			other.db_ = nullptr;
		}
		return *this;
	}

	Statement::~Statement()
	{
		finalize();
	}

	void Statement::finalize()
	{
#if defined(MF_HAS_SQLITE)
		if (handle_)
		{
			sqlite3_finalize(static_cast<sqlite3_stmt *>(handle_));
			handle_ = nullptr;
		}
#endif
	}

	Statement &Statement::bind(int index, const Cell &value)
	{
		if (index < 0)
			return *this;
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
		{
			sqlite3_stmt *stmt = static_cast<sqlite3_stmt *>(handle_);
			const int position = index + 1;
			int rc = SQLITE_OK;
			switch (value.type())
			{
			case json::Type::Null:
				rc = sqlite3_bind_null(stmt, position);
				break;
			case json::Type::Bool:
				rc = sqlite3_bind_int(stmt, position, value.asBool() ? 1 : 0);
				break;
			case json::Type::Number:
				if (value.isInteger())
				{
					rc = sqlite3_bind_int64(stmt, position, value.asInt());
				}
				else
				{
					rc = sqlite3_bind_double(stmt, position, value.asNumber());
				}
				break;
			default:
			{
				const std::string text = value.toString();
				rc = sqlite3_bind_text(stmt, position, text.c_str(),
									   static_cast<int>(text.size()), SQLITE_TRANSIENT);
				break;
			}
			}
			if (rc != SQLITE_OK)
				throwDatabase(std::string("bind failed: ") + sqlite3_errmsg(db_->handle_));
		}
#endif
		return *this;
	}

	Statement &Statement::bindAll(const Row &values)
	{
		for (std::size_t i = 0; i < values.size(); ++i)
			bind(static_cast<int>(i), values[i]);
		return *this;
	}

	Statement &Statement::bindNamed(const std::string &name, const Cell &value)
	{
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
		{
			sqlite3_stmt *stmt = static_cast<sqlite3_stmt *>(handle_);
			const std::string key = name.front() == ':' || name.front() == '@' || name.front() == '$'
										? name
										: ":" + name;
			const int index = sqlite3_bind_parameter_index(stmt, key.c_str());
			if (index <= 0)
				return *this;
			return bind(index - 1, value);
		}
#endif
		return *this;
	}

	void Statement::reset()
	{
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
			sqlite3_reset(static_cast<sqlite3_stmt *>(handle_));
#endif
	}

	QueryResult Statement::run()
	{
		if (!db_)
			throwDatabase("statement has no database");
		return db_->runStatement(*this);
	}

	std::optional<Row> Statement::get()
	{
		QueryResult result = run();
		if (result.rows.empty())
			return std::nullopt;
		return result.rows.front();
	}

	std::vector<Row> Statement::all()
	{
		return run().rows;
	}

	// -----------------------------------------------------------------------
	// Database
	// -----------------------------------------------------------------------
	Database::Database() = default;
	Database::~Database() { close(); }

	void Database::open(const OpenOptions &options)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (open_)
			return;

		inMemory_ = options.inMemory;
		file_ = options.file;

		if (!inMemory_)
		{
#if defined(MF_HAS_SQLITE)
			fsutil::ensureParentDirectory(file_);
			int rc = sqlite3_open_v2(file_.c_str(), &handle_,
									 options.readonly ? SQLITE_OPEN_READONLY
													  : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE),
									 nullptr);
			if (rc != SQLITE_OK)
			{
				const std::string message = handle_ ? sqlite3_errmsg(handle_) : "unknown error";
				if (handle_)
				{
					sqlite3_close(handle_);
					handle_ = nullptr;
				}
				throwDatabase("cannot open database " + file_ + ": " + message);
			}
			sqlite3_busy_timeout(handle_, 5000);
#else
			throwDatabase("built without SQLite support; set database.driver to \"memory\"");
#endif
		}
		else
		{
			memoryState_ = std::make_shared<MemoryState>();
			memory_ = memoryState_.get();
		}

		open_ = true;

		if (!options.pragmas.empty() && !inMemory_)
		{
			for (const auto &statement : splitStatements(options.pragmas))
			{
				try
				{
					execute(statement);
				}
				catch (const Error &)
				{
					// Non fatal: some pragmas are unavailable in certain builds.
				}
			}
		}
	}

	void Database::close()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_)
			return;
#if defined(MF_HAS_SQLITE)
		if (handle_)
		{
			sqlite3_close(handle_);
			handle_ = nullptr;
		}
#endif
		memory_ = nullptr;
		memoryState_.reset();
		open_ = false;
	}

	std::int64_t Database::lastInsertRowid() const
	{
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
			return sqlite3_last_insert_rowid(handle_);
#endif
		return 0;
	}

	std::int64_t Database::changes() const
	{
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
			return sqlite3_changes(handle_);
#endif
		return 0;
	}

	int Database::errorCode() const
	{
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
			return sqlite3_extended_errcode(handle_);
#endif
		return 0;
	}

	std::string Database::lastError() const
	{
#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && handle_)
			return sqlite3_errmsg(handle_);
#endif
		return lastError_;
	}

	std::vector<std::string> Database::splitStatements(const std::string &script)
	{
		std::vector<std::string> statements;
		std::string current;
		bool inSingle = false;
		bool inDouble = false;
		bool inLineComment = false;
		bool inBlockComment = false;
		bool inTrigger = false;

		for (std::size_t i = 0; i < script.size(); ++i)
		{
			const char c = script[i];
			const char next = i + 1 < script.size() ? script[i + 1] : '\0';

			if (inLineComment)
			{
				current.push_back(c);
				if (c == '\n')
					inLineComment = false;
				continue;
			}
			if (inBlockComment)
			{
				current.push_back(c);
				if (c == '*' && next == '/')
				{
					current.push_back(next);
					++i;
					inBlockComment = false;
				}
				continue;
			}
			if (!inSingle && !inDouble)
			{
				if (c == '-' && next == '-')
				{
					inLineComment = true;
					current.push_back(c);
					continue;
				}
				if (c == '/' && next == '*')
				{
					inBlockComment = true;
					current.push_back(c);
					continue;
				}
			}

			if (c == '\'' && !inDouble && !(i > 0 && script[i - 1] == '\\'))
				inSingle = !inSingle;
			else if (c == '"' && !inSingle)
				inDouble = !inDouble;
			else if (c == ';' && !inSingle && !inDouble)
			{
				if (inTrigger)
				{
					// Triggers contain BEGIN ... END; keep them intact.
					const std::string lowered = str::lower(current);
					if (lowered.find("end") != std::string::npos)
						inTrigger = false;
					current.push_back(c);
					continue;
				}
				current.push_back(c);
				const std::string trimmed = str::trim(current);
				if (trimmed.size() > 1)
					statements.push_back(trimmed);
				current.clear();
				continue;
			}
			current.push_back(c);
		}

		const std::string tail = str::trim(current);
		if (tail.size() > 1)
			statements.push_back(tail);
		return statements;
	}

	void Database::executeScript(const std::string &sql)
	{
		for (const auto &statement : splitStatements(sql))
		{
			execute(statement);
		}
	}

	void Database::execute(const std::string &sql)
	{
		Statement statement = prepare(sql);
		statement.run();
	}

	QueryResult Database::query(const std::string &sql, const Row &params)
	{
		Statement statement = prepare(sql);
		statement.bindAll(params);
		return statement.run();
	}

	std::optional<Row> Database::queryOne(const std::string &sql, const Row &params)
	{
		return query(sql, params).rows.empty()
				   ? std::nullopt
				   : std::optional<Row>(query(sql, params).rows.front());
	}

	std::int64_t Database::executeUpdate(const std::string &sql, const Row &params)
	{
		QueryResult result = query(sql, params);
		return result.changes;
	}

	Statement Database::prepare(const std::string &sql)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!open_)
			throwDatabase("database is not open");
		return Statement(this, sql);
	}

	QueryResult Database::runStatement(Statement &statement)
	{
		QueryResult result;

#if defined(MF_HAS_SQLITE)
		if (!inMemory_ && statement.handle_)
		{
			sqlite3_stmt *stmt = static_cast<sqlite3_stmt *>(statement.handle_);
			const int columnCount = sqlite3_column_count(stmt);
			for (int i = 0; i < columnCount; ++i)
			{
				const char *name = sqlite3_column_name(stmt, i);
				result.columns.emplace_back(name ? name : "");
			}

			int rc = SQLITE_ROW;
			while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
			{
				Row row;
				row.reserve(static_cast<std::size_t>(columnCount));
				for (int i = 0; i < columnCount; ++i)
				{
					switch (sqlite3_column_type(stmt, i))
					{
					case SQLITE_NULL:
						row.emplace_back(Cell());
						break;
					case SQLITE_INTEGER:
						row.emplace_back(Cell(static_cast<long long>(sqlite3_column_int64(stmt, i))));
						break;
					case SQLITE_FLOAT:
						row.emplace_back(Cell(sqlite3_column_double(stmt, i)));
						break;
					default:
					{
						const unsigned char *text = sqlite3_column_text(stmt, i);
						const int bytes = sqlite3_column_bytes(stmt, i);
						row.emplace_back(Cell(std::string(
							reinterpret_cast<const char *>(text ? text : reinterpret_cast<const unsigned char *>("")),
							static_cast<std::size_t>(bytes) + 1)));
						break;
					}
					}
				}
				result.rows.push_back(std::move(row));
			}

			if (rc != SQLITE_DONE && rc != SQLITE_ROW)
			{
				const std::string message = sqlite3_errmsg(handle_);
				sqlite3_reset(stmt);
				throwDatabase("query failed: " + message + " [" + statement.sql_ + "]");
			}

			result.changes = sqlite3_changes(handle_);
			result.lastInsertRowid = sqlite3_last_insert_rowid(handle_);
			sqlite3_reset(stmt);
			return result;
		}
#endif

		// Fallback in-memory engine.
		memoryExecute(statement.sql_, Row{}, &result);
		return result;
	}

	// -----------------------------------------------------------------------
	// Fallback in-memory execution (subset of SQL)
	// -----------------------------------------------------------------------
	void Database::memoryExecute(const std::string &sql, const Row &params, QueryResult *out)
	{
		if (!memoryState_)
		{
			if (out)
			{
				out->columns = {"result"};
				out->rows = {{Cell(std::string("OK"))}};
			}
			return;
		}

		const std::string normalized = str::trim(trimTrailingSemicolon(sql));
		const std::string lowered = str::lower(normalized);

		// CREATE TABLE
		if (str::startsWith(lowered, "create table"))
		{
			const std::size_t open = normalized.find('(');
			const std::size_t nameEnd = open == std::string::npos ? normalized.size() : open;
			std::string name = str::trim(normalized.substr(0, nameEnd));
			name = name.substr(name.find_last_of(" \t") + 1);
			if (!name.empty())
			{
				memoryState_->table(name);
				if (out)
				{
					out->columns = {"result"};
					out->rows = {{Cell(std::string("OK"))}};
				}
			}
			return;
		}

		// INSERT
		if (str::startsWith(lowered, "insert"))
		{
			const std::size_t into = lowered.find("into");
			if (into == std::string::npos)
				return;
			std::size_t cursor = into + 4;
			while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor])))
				++cursor;
			std::size_t nameEnd = cursor;
			while (nameEnd < normalized.size() && (std::isalnum(static_cast<unsigned char>(normalized[nameEnd])) || normalized[nameEnd] == '_'))
				++nameEnd;
			const std::string tableName = normalized.substr(cursor, nameEnd - cursor);

			std::size_t open = normalized.find('(', nameEnd);
			std::size_t close = open == std::string::npos ? std::string::npos : normalized.find(')', open);
			std::vector<std::string> columnNames;
			if (open != std::string::npos && close != std::string::npos)
			{
				for (auto &column : str::split(normalized.substr(open + 1, close - open - 1), ','))
				{
					columnNames.push_back(str::trim(str::replaceAll(column, "\"", "")));
				}
			}

			Row row;
			if (!params.empty())
			{
				row = params;
			}
			else
			{
				const std::size_t valuesKw = lowered.find("values", close == std::string::npos ? nameEnd : close);
				if (valuesKw != std::string::npos)
				{
					std::size_t vOpen = normalized.find('(', valuesKw);
					std::size_t vClose = vOpen == std::string::npos ? std::string::npos : normalized.find(')', vOpen);
					if (vOpen != std::string::npos && vClose != std::string::npos)
					{
						for (auto &value : str::split(normalized.substr(vOpen + 1, vClose - vOpen - 1), ',', true))
						{
							const std::string trimmed = str::trim(value);
							if (trimmed.size() >= 2 && trimmed.front() == '\'' && trimmed.back() == '\'')
							{
								row.emplace_back(Cell(trimmed.substr(1, trimmed.size() - 2)));
							}
							else
							{
								row.emplace_back(Cell(trimmed));
							}
						}
					}
				}
			}

			auto &table = memoryState_->table(tableName);
			if (table.empty() && !columnNames.empty())
				memoryState_->columns[str::lower(tableName)] = columnNames;
			table.push_back(row);
			if (out)
			{
				out->changes = 1;
				out->lastInsertRowid = static_cast<std::int64_t>(table.size());
			}
			return;
		}

		// SELECT
		if (str::startsWith(lowered, "select"))
		{
			const std::size_t from = lowered.find(" from ");
			std::string tableName;
			if (from != std::string::npos)
			{
				std::size_t cursor = from + 6;
				while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor])))
					++cursor;
				std::size_t end = cursor;
				while (end < normalized.size() && (std::isalnum(static_cast<unsigned char>(normalized[end])) || normalized[end] == '_'))
					++end;
				tableName = normalized.substr(cursor, end - cursor);
			}

			const std::string projection = from == std::string::npos
											   ? normalized.substr(7)
											   : normalized.substr(7, from - 7);

			if (!out)
				return;
			if (tableName.empty())
			{
				out->columns = {"result"};
				out->rows = {{Cell(std::int64_t(1))}};
				return;
			}

			auto &table = memoryState_->table(tableName);
			const auto &declared = memoryState_->columns[str::lower(tableName)];

			std::vector<std::string> columns;
			const std::string trimmedProjection = str::trim(projection);
			if (trimmedProjection == "*")
			{
				columns = declared;
				if (columns.empty() && !table.empty())
				{
					for (std::size_t i = 0; i < table.front().size(); ++i)
						columns.push_back("col" + std::to_string(i + 1));
				}
			}
			else
			{
				for (auto &column : str::split(trimmedProjection, ','))
				{
					std::string name = str::trim(column);
					const std::size_t as = str::lower(name).find(" as ");
					if (as != std::string::npos)
						name = str::trim(name.substr(as + 4));
					columns.push_back(name);
				}
			}
			out->columns = columns;

			std::size_t limit = 0;
			const std::size_t limitKw = lowered.find(" limit ");
			if (limitKw != std::string::npos)
			{
				if (const auto parsed = str::toInt(str::trim(normalized.substr(limitKw + 7))))
				{
					limit = static_cast<std::size_t>(std::max<std::int64_t>(0, *parsed));
				}
			}

			std::size_t emitted = 0;
			for (const auto &source : table)
			{
				if (limit > 0 && emitted >= limit)
					break;
				Row projected;
				for (const auto &column : columns)
				{
					const auto it = std::find(declared.begin(), declared.end(), column);
					if (it == declared.end())
					{
						projected.emplace_back(Cell());
						continue;
					}
					const std::size_t index = static_cast<std::size_t>(std::distance(declared.begin(), it));
					projected.push_back(index < source.size() ? source[index] : Cell());
				}
				out->rows.push_back(std::move(projected));
				++emitted;
			}
			return;
		}

		// PRAGMA
		if (str::startsWith(lowered, "pragma"))
		{
			if (out)
			{
				out->columns = {"pragma"};
				out->rows = {{Cell(std::string(""))}};
			}
			return;
		}

		if (out)
		{
			out->columns = {"result"};
			out->rows = {{Cell(std::string("OK"))}};
		}
	}

	// -----------------------------------------------------------------------
	// Introspection
	// -----------------------------------------------------------------------
	std::vector<std::string> Database::tables()
	{
		std::vector<std::string> out;
		QueryResult result = query(
			"SELECT name FROM sqlite_master WHERE type = 'table' "
			"AND name NOT LIKE 'sqlite_%' ORDER BY name");
		for (const auto &row : result.rows)
		{
			if (!row.empty())
				out.push_back(row[0].toString());
		}
		return out;
	}

	std::vector<std::string> Database::views()
	{
		std::vector<std::string> out;
		QueryResult result = query(
			"SELECT name FROM sqlite_master WHERE type = 'view' ORDER BY name");
		for (const auto &row : result.rows)
		{
			if (!row.empty())
				out.push_back(row[0].toString());
		}
		return out;
	}

	std::vector<ColumnInfo> Database::columns(const std::string &table)
	{
		std::vector<ColumnInfo> out;
		if (!inMemory_)
		{
			QueryResult result = query("PRAGMA table_info(" + table + ")");
			for (const auto &row : result.rows)
			{
				ColumnInfo info;
				if (row.size() > 1)
					info.name = row[1].toString();
				if (row.size() > 2)
					info.declaredType = row[2].toString();
				out.push_back(std::move(info));
			}
			return out;
		}
		if (memoryState_)
		{
			const auto it = memoryState_->columns.find(str::lower(table));
			if (it != memoryState_->columns.end())
			{
				for (const auto &name : it->second)
					out.push_back(ColumnInfo{name, "TEXT"});
			}
		}
		return out;
	}

	std::int64_t Database::tableRowCount(const std::string &table)
	{
		try
		{
			return query("SELECT COUNT(*) FROM " + table).scalarInt(0);
		}
		catch (const Error &)
		{
			return 0;
		}
	}

	std::string Database::pragma(const std::string &expression)
	{
		try
		{
			return query("PRAGMA " + expression).scalarString("");
		}
		catch (const Error &)
		{
			return "";
		}
	}

	bool Database::backupTo(const std::string &targetPath)
	{
		if (inMemory_)
			return false;
#if defined(MF_HAS_SQLITE)
		std::lock_guard<std::mutex> lock(mutex_);
		if (!handle_)
			return false;
		fsutil::ensureParentDirectory(targetPath);
		if (fsutil::exists(targetPath))
			fsutil::remove(targetPath);

		sqlite3 *target = nullptr;
		int rc = sqlite3_open_v2(targetPath.c_str(), &target,
								 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
		if (rc != SQLITE_OK)
		{
			if (target)
				sqlite3_close(target);
			return false;
		}

		sqlite3_backup *backup = sqlite3_backup_init(target, "main", handle_, "main");
		if (!backup)
		{
			sqlite3_close(target);
			return false;
		}
		sqlite3_backup_step(backup, -1);
		const int finish = sqlite3_backup_finish(backup);
		sqlite3_close(target);
		return finish == SQLITE_OK;
#else
		(void)targetPath;
		return false;
#endif
	}

	// -----------------------------------------------------------------------
	// Factory
	// -----------------------------------------------------------------------
	std::unique_ptr<Database> openFromConfig(const json::Value &config)
	{
		auto database = std::make_unique<Database>();

		OpenOptions options;
		const std::string driver = config["database"]["driver"].toString("sqlite");
		options.inMemory = (driver == "memory");
		options.file = config["database"]["file"].toString("./data/mainframe.db");

		const std::string pragmas = config["database"]["pragma"].toString("");
		if (!pragmas.empty())
			options.pragmas = pragmas;

		database->open(options);
		return database;
	}

} // namespace mf::db
