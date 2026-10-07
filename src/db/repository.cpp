#include "mf/db/repository.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

#include <algorithm>

namespace mf::db
{

	namespace
	{

		Cell text(const std::string &value) { return Cell(value); }
		Cell number(std::int64_t value) { return Cell(static_cast<long long>(value)); }

		// Rows come back positionally; this keeps the field mapping in one spot.
		json::Value rowToObject(const QueryResult &result, const Row &row,
								const std::vector<std::string> &fields)
		{
			json::Value object = json::Value::object();
			for (std::size_t i = 0; i < fields.size() && i < row.size(); ++i)
			{
				object.set(fields[i], row[i]);
			}
			return object;
		}

		std::int64_t insertOrFail(Database &database, const std::string &sql, const Row &params,
								  int lockThreshold, const std::string &what)
		{
			try
			{
				QueryResult result = database.query(sql, params);
				return result.lastInsertRowid;
			}
			catch (const Error &ex)
			{
				if (ex.code() == ErrorCode::Database && str::lower(ex.what()).find("unique") != std::string::npos)
				{
					throwConflict(what + " already exists");
				}
				throw;
			}
		}

		std::string columnList(const json::Value &fields, const std::vector<std::string> &allowed)
		{
			std::string out;
			for (const auto &name : allowed)
			{
				if (!fields.has(name))
					continue;
				if (!out.empty())
					out += ", ";
				out += name + " = ?";
			}
			return out;
		}

		Row columnValues(const json::Value &fields, const std::vector<std::string> &allowed)
		{
			Row out;
			for (const auto &name : allowed)
			{
				if (!fields.has(name))
					continue;
				out.push_back(fields[name]);
			}
			return out;
		}

	} // namespace

	std::string buildOrderClause(const std::optional<std::string> &column,
								 const std::vector<std::string> &allowed,
								 bool descending,
								 const std::string &fallback)
	{
		std::string chosen = fallback;
		if (column && !column->empty())
		{
			for (const auto &candidate : allowed)
			{
				if (str::iequals(candidate, *column))
				{
					chosen = candidate;
					break;
				}
			}
		}
		return "ORDER BY " + chosen + (descending ? " DESC" : " ASC");
	}

	// =======================================================================
	// Users
	// =======================================================================
	namespace
	{
		const std::vector<std::string> USER_FIELDS = {
			"user_id", "username", "full_name", "department", "role", "status",
			"failed_logons", "last_logon", "created_at", "updated_at"};
	}

	json::Array UserRepository::list(const Page &page)
	{
		const std::string order = buildOrderClause(page.orderBy,
												   {"user_id", "username", "role", "status", "created_at"}, page.descending, "user_id");
		const QueryResult result = database_.query(
			"SELECT user_id, username, full_name, department, role, status, failed_logons, "
			"last_logon, created_at, updated_at FROM users " +
				order + " LIMIT ? OFFSET ?",
			{number(page.limit), number(page.offset)});

		json::Array out;
		for (const auto &row : result.rows)
			out.push_back(rowToObject(result, row, USER_FIELDS));
		return out;
	}

	std::int64_t UserRepository::count()
	{
		return database_.query("SELECT COUNT(*) FROM users").scalarInt(0);
	}

	std::optional<json::Value> UserRepository::findById(std::int64_t userId)
	{
		const QueryResult result = database_.query(
			"SELECT user_id, username, full_name, department, role, status, failed_logons, "
			"last_logon, created_at, updated_at FROM users WHERE user_id = ?",
			{number(userId)});
		if (result.rows.empty())
			return std::nullopt;
		return rowToObject(result, result.rows.front(), USER_FIELDS);
	}

	std::optional<json::Value> UserRepository::findByUsername(const std::string &username)
	{
		const QueryResult result = database_.query(
			"SELECT user_id, username, full_name, department, role, status, failed_logons, "
			"last_logon, created_at, updated_at, password_hash FROM users WHERE username = ?",
			{text(str::upper(username))});
		if (result.rows.empty())
			return std::nullopt;
		std::vector<std::string> fields = USER_FIELDS;
		fields.push_back("password_hash");
		return rowToObject(result, result.rows.front(), fields);
	}

	json::Array UserRepository::findByRole(const std::string &role)
	{
		const QueryResult result = database_.query(
			"SELECT user_id, username, full_name, department, role, status, failed_logons, "
			"last_logon, created_at, updated_at FROM users WHERE role = ? ORDER BY username",
			{text(str::upper(role))});
		json::Array out;
		for (const auto &row : result.rows)
			out.push_back(rowToObject(result, row, USER_FIELDS));
		return out;
	}

	std::int64_t UserRepository::create(const json::Value &user)
	{
		return insertOrFail(database_,
							"INSERT INTO users (username, full_name, department, role, status, password_hash) "
							"VALUES (?, ?, ?, ?, ?, ?)",
							{text(str::upper(user["username"].toString())),
							 text(user["full_name"].toString("")),
							 text(str::upper(user["department"].toString("GENERAL"))),
							 text(str::upper(user["role"].toString("OPERATOR"))),
							 text(str::upper(user["status"].toString("ACTIVE"))),
							 text(user["password_hash"].toString(""))},
							5, "user");
	}

	bool UserRepository::update(std::int64_t userId, const json::Value &fields)
	{
		const std::vector<std::string> allowed = {"full_name", "department", "role", "status"};
		const std::string assignments = columnList(fields, allowed);
		if (assignments.empty())
			return false;
		Row params = columnValues(fields, allowed);
		params.push_back(number(userId));
		const std::int64_t changes = database_.executeUpdate(
			"UPDATE users SET " + assignments + ", updated_at = datetime('now') WHERE user_id = ?", params);
		return changes > 0;
	}

	bool UserRepository::remove(std::int64_t userId)
	{
		return database_.executeUpdate("DELETE FROM users WHERE user_id = ?", {number(userId)}) > 0;
	}

	bool UserRepository::recordLogon(std::int64_t userId)
	{
		return database_.executeUpdate(
				   "UPDATE users SET last_logon = datetime('now'), failed_logons = 0, "
				   "updated_at = datetime('now') WHERE user_id = ?",
				   {number(userId)}) > 0;
	}

	bool UserRepository::recordFailedLogon(std::int64_t userId, int lockThreshold)
	{
		database_.executeUpdate(
			"UPDATE users SET failed_logons = failed_logons + 1, updated_at = datetime('now') "
			"WHERE user_id = ?",
			{number(userId)});

		if (lockThreshold > 0)
		{
			database_.executeUpdate(
				"UPDATE users SET status = 'LOCKED' WHERE user_id = ? AND failed_logons >= ?",
				{number(userId), number(lockThreshold)});
		}
		return true;
	}

	bool UserRepository::resetFailedLogons(std::int64_t userId)
	{
		return database_.executeUpdate(
				   "UPDATE users SET failed_logons = 0 WHERE user_id = ?", {number(userId)}) > 0;
	}

	bool UserRepository::setStatus(std::int64_t userId, const std::string &status)
	{
		return database_.executeUpdate(
				   "UPDATE users SET status = ?, updated_at = datetime('now') WHERE user_id = ?",
				   {text(str::upper(status)), number(userId)}) > 0;
	}

	bool UserRepository::setPasswordHash(std::int64_t userId, const std::string &hash)
	{
		return database_.executeUpdate(
				   "UPDATE users SET password_hash = ?, updated_at = datetime('now') WHERE user_id = ?",
				   {text(hash), number(userId)}) > 0;
	}

	std::int64_t UserRepository::countByRole(const std::string &role)
	{
		return database_.query("SELECT COUNT(*) FROM users WHERE role = ?",
							   {text(str::upper(role))})
			.scalarInt(0);
	}

	// =======================================================================
	// Datasets
	// =======================================================================
	namespace
	{
		const std::vector<std::string> DATASET_FIELDS = {
			"dataset_id", "name", "dsorg", "recfm", "lrecl", "blksize", "volume",
			"owner_user_id", "record_count", "bytes_used", "status", "created_at", "updated_at"};
	}

	json::Array DatasetRepository::list(const Page &page)
	{
		const std::string order = buildOrderClause(page.orderBy,
												   {"dataset_id", "name", "record_count", "bytes_used", "created_at"}, page.descending, "name");
		const QueryResult result = database_.query(
			"SELECT d.dataset_id, d.name, d.dsorg, d.recfm, d.lrecl, d.blksize, d.volume, "
			"d.owner_user_id, d.record_count, d.bytes_used, d.status, d.created_at, d.updated_at, "
			"u.username AS owner_name "
			"FROM datasets d LEFT JOIN users u ON u.user_id = d.owner_user_id " +
				order + " LIMIT ? OFFSET ?",
			{number(page.limit), number(page.offset)});

		std::vector<std::string> fields = DATASET_FIELDS;
		fields.push_back("owner_name");
		json::Array out;
		for (const auto &row : result.rows)
			out.push_back(rowToObject(result, row, fields));
		return out;
	}

	json::Array DatasetRepository::search(const std::string &pattern, const Page &page)
	{
		const std::string like = "%" + str::upper(pattern) + "%";
		const QueryResult result = database_.query(
			"SELECT d.dataset_id, d.name, d.dsorg, d.recfm, d.lrecl, d.blksize, d.volume, "
			"d.owner_user_id, d.record_count, d.bytes_used, d.status, d.created_at, d.updated_at, "
			"u.username AS owner_name "
			"FROM datasets d LEFT JOIN users u ON u.user_id = d.owner_user_id "
			"WHERE d.name LIKE ? ORDER BY d.name LIMIT ? OFFSET ?",
			{text(like), number(page.limit), number(page.offset)});

		std::vector<std::string> fields = DATASET_FIELDS;
		fields.push_back("owner_name");
		json::Array out;
		for (const auto &row : result.rows)
			out.push_back(rowToObject(result, row, fields));
		return out;
	}

	std::int64_t DatasetRepository::count()
	{
		return database_.query("SELECT COUNT(*) FROM datasets").scalarInt(0);
	}

	std::optional<json::Value> DatasetRepository::findById(std::int64_t datasetId)
	{
		const QueryResult result = database_.query(
			"SELECT d.dataset_id, d.name, d.dsorg, d.recfm, d.lrecl, d.blksize, d.volume, "
			"d.owner_user_id, d.record_count, d.bytes_used, d.status, d.created_at, d.updated_at, "
			"u.username AS owner_name "
			"FROM datasets d LEFT JOIN users u ON u.user_id = d.owner_user_id WHERE d.dataset_id = ?",
			{number(datasetId)});
		if (result.rows.empty())
			return std::nullopt;
		std::vector<std::string> fields = DATASET_FIELDS;
		fields.push_back("owner_name");
		return rowToObject(result, result.rows.front(), fields);
	}

	std::optional<json::Value> DatasetRepository::findByName(const std::string &name)
	{
		const QueryResult result = database_.query(
			"SELECT dataset_id, name, dsorg, recfm, lrecl, blksize, volume, owner_user_id, "
			"record_count, bytes_used, status, created_at, updated_at "
			"FROM datasets WHERE name = ?",
			{text(str::upper(name))});
		if (result.rows.empty())
			return std::nullopt;
		return rowToObject(result, result.rows.front(), DATASET_FIELDS);
	}

	bool DatasetRepository::exists(const std::string &name)
	{
		return database_.query("SELECT COUNT(*) FROM datasets WHERE name = ?",
							   {text(str::upper(name))})
				   .scalarInt(0) > 0;
	}

	std::int64_t DatasetRepository::create(const json::Value &dataset)
	{
		return insertOrFail(database_,
							"INSERT INTO datasets (name, dsorg, recfm, lrecl, blksize, volume, owner_user_id, status) "
							"VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
							{text(str::upper(dataset["name"].toString())),
							 text(str::upper(dataset["dsorg"].toString("PS"))),
							 text(str::upper(dataset["recfm"].toString("FB"))),
							 number(dataset["lrecl"].toInt(80)),
							 number(dataset["blksize"].toInt(27920)),
							 text(str::upper(dataset["volume"].toString("MFVOL1"))),
							 dataset["owner_user_id"].isNull() ? Cell() : number(dataset["owner_user_id"].toInt(0)),
							 text(str::upper(dataset["status"].toString("AVAILABLE")))},
							5, "dataset");
	}

	bool DatasetRepository::update(std::int64_t datasetId, const json::Value &fields)
	{
		const std::vector<std::string> allowed = {"dsorg", "recfm", "lrecl", "blksize", "volume", "owner_user_id"};
		const std::string assignments = columnList(fields, allowed);
		if (assignments.empty())
			return false;
		Row params = columnValues(fields, allowed);
		params.push_back(number(datasetId));
		return database_.executeUpdate(
				   "UPDATE datasets SET " + assignments + ", updated_at = datetime('now') WHERE dataset_id = ?",
				   params) > 0;
	}

	bool DatasetRepository::remove(std::int64_t datasetId)
	{
		return database_.executeUpdate("DELETE FROM datasets WHERE dataset_id = ?",
									   {number(datasetId)}) > 0;
	}

	bool DatasetRepository::setStatus(std::int64_t datasetId, const std::string &status)
	{
		return database_.executeUpdate(
				   "UPDATE datasets SET status = ?, updated_at = datetime('now') WHERE dataset_id = ?",
				   {text(str::upper(status)), number(datasetId)}) > 0;
	}

	bool DatasetRepository::migrateToVolume(std::int64_t datasetId, const std::string &volume)
	{
		return database_.executeUpdate(
				   "UPDATE datasets SET volume = ?, status = 'MIGRATED', updated_at = datetime('now') "
				   "WHERE dataset_id = ?",
				   {text(str::upper(volume)), number(datasetId)}) > 0;
	}

	bool DatasetRepository::updateCounters(std::int64_t datasetId)
	{
		return database_.executeUpdate(
				   "UPDATE datasets SET "
				   "  record_count = (SELECT COUNT(*) FROM dataset_records r WHERE r.dataset_id = datasets.dataset_id), "
				   "  bytes_used   = COALESCE((SELECT SUM(LENGTH(r.payload)) FROM dataset_records r "
				   "                           WHERE r.dataset_id = datasets.dataset_id), 0), "
				   "  updated_at   = datetime('now') "
				   "WHERE dataset_id = ?",
				   {number(datasetId)}) >= 0;
	}

	bool DatasetRepository::transferOwner(std::int64_t datasetId, std::int64_t newOwnerId)
	{
		return database_.executeUpdate(
				   "UPDATE datasets SET owner_user_id = ?, updated_at = datetime('now') WHERE dataset_id = ?",
				   {number(newOwnerId), number(datasetId)}) > 0;
	}

	std::int64_t DatasetRepository::totalRecords()
	{
		return database_.query("SELECT COALESCE(SUM(record_count), 0) FROM datasets").scalarInt(0);
	}

	std::int64_t DatasetRepository::totalBytes()
	{
		return database_.query("SELECT COALESCE(SUM(bytes_used), 0) FROM datasets").scalarInt(0);
	}

	json::Array DatasetRepository::byVolume()
	{
		const QueryResult result = database_.query(
			"SELECT volume, COUNT(*) AS dataset_count, COALESCE(SUM(record_count),0) AS records, "
			"COALESCE(SUM(bytes_used),0) AS bytes_used "
			"FROM datasets WHERE status <> 'DELETED' GROUP BY volume ORDER BY bytes_used DESC");
		return result.toJson();
	}

	json::Array DatasetRepository::byOwner(std::int64_t ownerId)
	{
		const QueryResult result = database_.query(
			"SELECT dataset_id, name, dsorg, recfm, record_count, bytes_used, status "
			"FROM datasets WHERE owner_user_id = ? ORDER BY name",
			{number(ownerId)});
		return result.toJson();
	}

	// =======================================================================
	// Records
	// =======================================================================
	json::Array RecordRepository::list(std::int64_t datasetId, std::int64_t limit, std::int64_t offset)
	{
		const QueryResult result = database_.query(
			"SELECT record_id, dataset_id, sequence, payload, created_at "
			"FROM dataset_records WHERE dataset_id = ? ORDER BY sequence LIMIT ? OFFSET ?",
			{number(datasetId), number(limit), number(offset)});
		return result.toJson();
	}

	std::int64_t RecordRepository::count(std::int64_t datasetId)
	{
		return database_.query("SELECT COUNT(*) FROM dataset_records WHERE dataset_id = ?",
							   {number(datasetId)})
			.scalarInt(0);
	}

	std::optional<json::Value> RecordRepository::findById(std::int64_t recordId)
	{
		const QueryResult result = database_.query(
			"SELECT record_id, dataset_id, sequence, payload, created_at "
			"FROM dataset_records WHERE record_id = ?",
			{number(recordId)});
		if (result.rows.empty())
			return std::nullopt;
		return result.rows.front().empty() ? std::nullopt
										   : std::optional<json::Value>(result.toJson().front());
	}

	std::int64_t RecordRepository::nextSequence(std::int64_t datasetId)
	{
		return database_.query(
							"SELECT COALESCE(MAX(sequence), 0) + 1 FROM dataset_records WHERE dataset_id = ?",
							{number(datasetId)})
			.scalarInt(1);
	}

	std::int64_t RecordRepository::append(std::int64_t datasetId, const std::string &payload)
	{
		const std::int64_t sequence = nextSequence(datasetId);
		const QueryResult result = database_.query(
			"INSERT INTO dataset_records (dataset_id, sequence, payload) VALUES (?, ?, ?)",
			{number(datasetId), number(sequence), text(payload)});
		return result.lastInsertRowid;
	}

	std::int64_t RecordRepository::appendMany(std::int64_t datasetId, const std::vector<std::string> &payloads)
	{
		std::int64_t sequence = nextSequence(datasetId);
		std::int64_t inserted = 0;
		for (const auto &payload : payloads)
		{
			database_.query(
				"INSERT INTO dataset_records (dataset_id, sequence, payload) VALUES (?, ?, ?)",
				{number(datasetId), number(sequence), text(payload)});
			++sequence;
			++inserted;
		}
		return inserted;
	}

	std::int64_t RecordRepository::replaceAll(std::int64_t datasetId, const std::vector<std::string> &payloads)
	{
		removeAll(datasetId);
		return appendMany(datasetId, payloads);
	}

	bool RecordRepository::remove(std::int64_t recordId)
	{
		return database_.executeUpdate("DELETE FROM dataset_records WHERE record_id = ?",
									   {number(recordId)}) > 0;
	}

	std::int64_t RecordRepository::removeAll(std::int64_t datasetId)
	{
		return database_.executeUpdate("DELETE FROM dataset_records WHERE dataset_id = ?",
									   {number(datasetId)});
	}

	std::string RecordRepository::exportPayload(std::int64_t datasetId)
	{
		const QueryResult result = database_.query(
			"SELECT payload FROM dataset_records WHERE dataset_id = ? ORDER BY sequence",
			{number(datasetId)});
		std::string out;
		for (const auto &row : result.rows)
		{
			if (!row.empty())
			{
				out += row[0].toString();
				out += "\n";
			}
		}
		return out;
	}

	// =======================================================================
	// Jobs
	// =======================================================================
	namespace
	{
		const std::vector<std::string> JOB_FIELDS = {
			"job_id", "job_name", "job_class", "step_name", "program", "submitted_by",
			"status", "return_code", "priority", "submitted_at", "started_at", "ended_at",
			"cpu_ms", "output"};
	}

	json::Array JobRepository::list(const Page &page)
	{
		const std::string order = buildOrderClause(page.orderBy,
												   {"job_id", "job_name", "status", "priority", "submitted_at"}, page.descending, "job_id");
		const QueryResult result = database_.query(
			"SELECT j.job_id, j.job_name, j.job_class, j.step_name, j.program, j.submitted_by, "
			"j.status, j.return_code, j.priority, j.submitted_at, j.started_at, j.ended_at, "
			"j.cpu_ms, j.output, u.username AS submitter "
			"FROM jobs j LEFT JOIN users u ON u.user_id = j.submitted_by " +
				order + " LIMIT ? OFFSET ?",
			{number(page.limit), number(page.offset)});

		std::vector<std::string> fields = JOB_FIELDS;
		fields.push_back("submitter");
		json::Array out;
		for (const auto &row : result.rows)
			out.push_back(rowToObject(result, row, fields));
		return out;
	}

	json::Array JobRepository::byStatus(const std::string &status, std::int64_t limit)
	{
		const QueryResult result = database_.query(
			"SELECT job_id, job_name, job_class, status, return_code, priority, submitted_at, "
			"started_at, ended_at, cpu_ms FROM jobs WHERE status = ? "
			"ORDER BY priority DESC, submitted_at LIMIT ?",
			{text(str::upper(status)), number(limit)});
		return result.toJson();
	}

	json::Array JobRepository::queue()
	{
		const QueryResult result = database_.query(
			"SELECT job_id, job_name, job_class, status, priority, submitted_at "
			"FROM jobs WHERE status IN ('QUEUED','RUNNING','HOLD') "
			"ORDER BY priority DESC, submitted_at ASC");
		return result.toJson();
	}

	std::int64_t JobRepository::count()
	{
		return database_.query("SELECT COUNT(*) FROM jobs").scalarInt(0);
	}

	std::optional<json::Value> JobRepository::findById(std::int64_t jobId)
	{
		const QueryResult result = database_.query(
			"SELECT j.job_id, j.job_name, j.job_class, j.step_name, j.program, j.submitted_by, "
			"j.status, j.return_code, j.priority, j.submitted_at, j.started_at, j.ended_at, "
			"j.cpu_ms, j.output, u.username AS submitter "
			"FROM jobs j LEFT JOIN users u ON u.user_id = j.submitted_by WHERE j.job_id = ?",
			{number(jobId)});
		if (result.rows.empty())
			return std::nullopt;
		std::vector<std::string> fields = JOB_FIELDS;
		fields.push_back("submitter");
		return rowToObject(result, result.rows.front(), fields);
	}

	std::int64_t JobRepository::countByStatus(const std::string &status)
	{
		return database_.query("SELECT COUNT(*) FROM jobs WHERE status = ?",
							   {text(str::upper(status))})
			.scalarInt(0);
	}

	std::int64_t JobRepository::create(const json::Value &job)
	{
		return insertOrFail(database_,
							"INSERT INTO jobs (job_name, job_class, step_name, program, submitted_by, status, priority) "
							"VALUES (?, ?, ?, ?, ?, 'QUEUED', ?)",
							{text(str::upper(job["job_name"].toString("JOB"))),
							 text(str::upper(job["job_class"].toString("A"))),
							 text(str::upper(job["step_name"].toString("STEP0001"))),
							 text(str::upper(job["program"].toString("IEFBR14"))),
							 job["submitted_by"].isNull() ? Cell() : number(job["submitted_by"].toInt(0)),
							 number(job["priority"].toInt(5))},
							5, "job");
	}

	bool JobRepository::markRunning(std::int64_t jobId)
	{
		return database_.executeUpdate(
				   "UPDATE jobs SET status = 'RUNNING', started_at = datetime('now') WHERE job_id = ?",
				   {number(jobId)}) > 0;
	}

	bool JobRepository::markComplete(std::int64_t jobId, int returnCode, std::int64_t cpuMs,
									 const std::string &output)
	{
		return database_.executeUpdate(
				   "UPDATE jobs SET status = ?, return_code = ?, cpu_ms = ?, output = ?, "
				   "ended_at = datetime('now') WHERE job_id = ?",
				   {text(returnCode == 0 ? "COMPLETE" : "ABEND"),
					number(returnCode), number(cpuMs), text(output), number(jobId)}) > 0;
	}

	bool JobRepository::markAbend(std::int64_t jobId, int returnCode, const std::string &output)
	{
		return database_.executeUpdate(
				   "UPDATE jobs SET status = 'ABEND', return_code = ?, output = ?, "
				   "ended_at = datetime('now') WHERE job_id = ?",
				   {number(returnCode), text(output), number(jobId)}) > 0;
	}

	bool JobRepository::cancel(std::int64_t jobId)
	{
		return database_.executeUpdate(
				   "UPDATE jobs SET status = 'CANCELLED', ended_at = datetime('now') "
				   "WHERE job_id = ? AND status IN ('QUEUED','RUNNING','HOLD')",
				   {number(jobId)}) > 0;
	}

	bool JobRepository::hold(std::int64_t jobId)
	{
		return database_.executeUpdate(
				   "UPDATE jobs SET status = 'HOLD' WHERE job_id = ? AND status = 'QUEUED'",
				   {number(jobId)}) > 0;
	}

	bool JobRepository::release(std::int64_t jobId)
	{
		return database_.executeUpdate(
				   "UPDATE jobs SET status = 'QUEUED' WHERE job_id = ? AND status = 'HOLD'",
				   {number(jobId)}) > 0;
	}

	bool JobRepository::purge(const std::string &status, int olderThanDays)
	{
		const std::string sql =
			"DELETE FROM jobs WHERE status = ? AND ended_at IS NOT NULL "
			"AND ended_at < datetime('now', ?)";
		return database_.executeUpdate(sql, {text(str::upper(status)),
											 text("-" + std::to_string(olderThanDays) + " days")}) >= 0;
	}

	json::Value JobRepository::summary()
	{
		const QueryResult result = database_.query(
			"SELECT status, COUNT(*) AS job_count, COALESCE(SUM(cpu_ms),0) AS total_cpu_ms, "
			"SUM(CASE WHEN return_code <> 0 THEN 1 ELSE 0 END) AS failures "
			"FROM jobs GROUP BY status ORDER BY status");

		json::Value out = json::Value::object();
		out.set("byStatus", json::Value(result.toJson()));
		out.set("total", json::Value(static_cast<long long>(count())));
		out.set("queued", json::Value(static_cast<long long>(countByStatus("QUEUED"))));
		out.set("running", json::Value(static_cast<long long>(countByStatus("RUNNING"))));
		out.set("abend", json::Value(static_cast<long long>(countByStatus("ABEND"))));
		return out;
	}

	// =======================================================================
	// Transactions
	// =======================================================================
	json::Array TransactionRepository::list(const Page &page)
	{
		const std::string order = buildOrderClause(page.orderBy,
												   {"txn_id", "txn_code", "status", "created_at", "elapsed_ms"}, page.descending, "txn_id");
		const QueryResult result = database_.query(
			"SELECT t.txn_id, t.txn_code, t.terminal, t.user_id, t.status, t.rows_read, "
			"t.rows_written, t.elapsed_ms, t.detail, t.created_at, u.username AS username "
			"FROM transactions t LEFT JOIN users u ON u.user_id = t.user_id " +
				order + " LIMIT ? OFFSET ?",
			{number(page.limit), number(page.offset)});
		return result.toJson();
	}

	std::int64_t TransactionRepository::count()
	{
		return database_.query("SELECT COUNT(*) FROM transactions").scalarInt(0);
	}

	std::optional<json::Value> TransactionRepository::findById(std::int64_t txnId)
	{
		const QueryResult result = database_.query(
			"SELECT txn_id, txn_code, terminal, user_id, status, rows_read, rows_written, "
			"elapsed_ms, detail, created_at FROM transactions WHERE txn_id = ?",
			{number(txnId)});
		if (result.rows.empty())
			return std::nullopt;
		return result.toJson().front();
	}

	json::Array TransactionRepository::byCode(const std::string &txnCode, std::int64_t limit)
	{
		const QueryResult result = database_.query(
			"SELECT txn_id, txn_code, terminal, status, rows_read, rows_written, elapsed_ms, "
			"detail, created_at FROM transactions WHERE txn_code = ? ORDER BY txn_id DESC LIMIT ?",
			{text(str::upper(txnCode)), number(limit)});
		return result.toJson();
	}

	json::Array TransactionRepository::recentFailures(std::int64_t limit)
	{
		const QueryResult result = database_.query(
			"SELECT txn_id, txn_code, terminal, status, detail, created_at FROM transactions "
			"WHERE status IN ('FAIL','ABEND') ORDER BY txn_id DESC LIMIT ?",
			{number(limit)});
		return result.toJson();
	}

	std::int64_t TransactionRepository::create(const json::Value &transaction)
	{
		return insertOrFail(database_,
							"INSERT INTO transactions (txn_code, terminal, user_id, status, rows_read, rows_written, "
							"elapsed_ms, detail) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
							{text(str::upper(transaction["txn_code"].toString("UNKN"))),
							 text(transaction["terminal"].toString("L3270")),
							 transaction["user_id"].isNull() ? Cell() : number(transaction["user_id"].toInt(0)),
							 text(str::upper(transaction["status"].toString("OK"))),
							 number(transaction["rows_read"].toInt(0)),
							 number(transaction["rows_written"].toInt(0)),
							 number(transaction["elapsed_ms"].toInt(0)),
							 text(transaction["detail"].toString(""))},
							5, "transaction");
	}

	bool TransactionRepository::record(const std::string &txnCode, const std::string &terminal,
									   std::int64_t userId, const std::string &status,
									   std::int64_t rowsRead, std::int64_t rowsWritten,
									   std::int64_t elapsedMs, const std::string &detail)
	{
		json::Value transaction = json::Value::object();
		transaction.set("txn_code", json::Value(txnCode));
		transaction.set("terminal", json::Value(terminal));
		transaction.set("user_id", userId > 0 ? json::Value(static_cast<long long>(userId)) : json::Value());
		transaction.set("status", json::Value(status));
		transaction.set("rows_read", json::Value(static_cast<long long>(rowsRead)));
		transaction.set("rows_written", json::Value(static_cast<long long>(rowsWritten)));
		transaction.set("elapsed_ms", json::Value(static_cast<long long>(elapsedMs)));
		transaction.set("detail", json::Value(detail));
		return create(transaction) > 0;
	}

	json::Value TransactionRepository::activityReport()
	{
		const QueryResult result = database_.query(
			"SELECT txn_code, COUNT(*) AS executions, COALESCE(SUM(rows_read),0) AS rows_read, "
			"COALESCE(SUM(rows_written),0) AS rows_written, ROUND(AVG(elapsed_ms), 1) AS avg_elapsed_ms, "
			"SUM(CASE WHEN status = 'OK' THEN 1 ELSE 0 END) AS ok_count, "
			"SUM(CASE WHEN status <> 'OK' THEN 1 ELSE 0 END) AS problem_count "
			"FROM transactions GROUP BY txn_code ORDER BY executions DESC");
		return json::Value(result.toJson());
	}

	// =======================================================================
	// Audit
	// =======================================================================
	json::Array AuditRepository::list(const Page &page)
	{
		const std::string order = buildOrderClause(page.orderBy,
												   {"log_id", "event_type", "severity", "created_at"}, page.descending, "log_id");
		const QueryResult result = database_.query(
			"SELECT log_id, event_type, severity, actor, resource, message, metadata, created_at "
			"FROM audit_log " +
				order + " LIMIT ? OFFSET ?",
			{number(page.limit), number(page.offset)});
		return result.toJson();
	}

	json::Array AuditRepository::recent(std::int64_t limit)
	{
		const QueryResult result = database_.query(
			"SELECT log_id, event_type, severity, actor, resource, message, metadata, created_at "
			"FROM audit_log ORDER BY log_id DESC LIMIT ?",
			{number(limit)});
		return result.toJson();
	}

	json::Array AuditRepository::byActor(const std::string &actor, std::int64_t limit)
	{
		const QueryResult result = database_.query(
			"SELECT log_id, event_type, severity, actor, resource, message, created_at "
			"FROM audit_log WHERE actor = ? ORDER BY log_id DESC LIMIT ?",
			{text(str::upper(actor)), number(limit)});
		return result.toJson();
	}

	json::Array AuditRepository::search(const std::string &value, std::int64_t limit)
	{
		const std::string like = "%" + value + "%";
		const QueryResult result = database_.query(
			"SELECT log_id, event_type, severity, actor, resource, message, created_at "
			"FROM audit_log WHERE message LIKE ? OR resource LIKE ? OR actor LIKE ? "
			"ORDER BY log_id DESC LIMIT ?",
			{text(like), text(like), text(like), number(limit)});
		return result.toJson();
	}

	std::int64_t AuditRepository::count()
	{
		return database_.query("SELECT COUNT(*) FROM audit_log").scalarInt(0);
	}

	std::int64_t AuditRepository::write(const std::string &eventType, const std::string &severity,
										const std::string &actor, const std::string &resource,
										const std::string &message, const json::Value &metadata)
	{
		const QueryResult result = database_.query(
			"INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata) "
			"VALUES (?, ?, ?, ?, ?, ?)",
			{text(str::upper(eventType)),
			 text(str::upper(severity)),
			 text(actor.empty() ? "SYSTEM" : actor),
			 text(resource),
			 text(message),
			 text(metadata.isObject() || metadata.isArray() ? metadata.dumpCompact() : "{}")});
		return result.lastInsertRowid;
	}

	std::int64_t AuditRepository::purgeOlderThan(int days)
	{
		return database_.executeUpdate(
			"DELETE FROM audit_log WHERE created_at < datetime('now', ?)",
			{text("-" + std::to_string(days) + " days")});
	}

	json::Value AuditRepository::summaryByDay(int days)
	{
		const QueryResult result = database_.query(
			"SELECT date(created_at) AS day, severity, COUNT(*) AS events FROM audit_log "
			"WHERE created_at >= datetime('now', ?) GROUP BY day, severity ORDER BY day DESC, severity",
			{text("-" + std::to_string(days) + " days")});
		return json::Value(result.toJson());
	}

	json::Value AuditRepository::summaryBySeverity()
	{
		const QueryResult result = database_.query(
			"SELECT severity, COUNT(*) AS events FROM audit_log GROUP BY severity ORDER BY severity");
		return json::Value(result.toJson());
	}

	// =======================================================================
	// Volumes
	// =======================================================================
	json::Array VolumeRepository::list()
	{
		const QueryResult result = database_.query(
			"SELECT volume_id, serial, device_type, capacity_mb, used_mb, status, created_at "
			"FROM volumes ORDER BY serial");
		return result.toJson();
	}

	std::optional<json::Value> VolumeRepository::findBySerial(const std::string &serial)
	{
		const QueryResult result = database_.query(
			"SELECT volume_id, serial, device_type, capacity_mb, used_mb, status, created_at "
			"FROM volumes WHERE serial = ?",
			{text(str::upper(serial))});
		if (result.rows.empty())
			return std::nullopt;
		return result.toJson().front();
	}

	std::int64_t VolumeRepository::create(const json::Value &volume)
	{
		return insertOrFail(database_,
							"INSERT INTO volumes (serial, device_type, capacity_mb, used_mb, status) VALUES (?, ?, ?, ?, ?)",
							{text(str::upper(volume["serial"].toString())),
							 text(volume["device_type"].toString("3390")),
							 number(volume["capacity_mb"].toInt(1024)),
							 number(volume["used_mb"].toInt(0)),
							 text(str::upper(volume["status"].toString("ONLINE")))},
							5, "volume");
	}

	bool VolumeRepository::updateUsage(const std::string &serial, std::int64_t usedMb)
	{
		return database_.executeUpdate(
				   "UPDATE volumes SET used_mb = ?, "
				   "status = CASE WHEN ? >= capacity_mb THEN 'FULL' ELSE status END WHERE serial = ?",
				   {number(usedMb), number(usedMb), text(str::upper(serial))}) > 0;
	}

	json::Array VolumeRepository::utilization()
	{
		const QueryResult result = database_.query(
			"SELECT v.serial, v.device_type, v.capacity_mb, v.used_mb, v.status, "
			"  (v.capacity_mb - v.used_mb) AS free_mb, "
			"  ROUND(100.0 * v.used_mb / NULLIF(v.capacity_mb, 0), 2) AS pct_used, "
			"  COUNT(d.dataset_id) AS dataset_count "
			"FROM volumes v LEFT JOIN datasets d ON d.volume = v.serial AND d.status <> 'DELETED' "
			"GROUP BY v.volume_id ORDER BY pct_used DESC");
		return result.toJson();
	}

	// =======================================================================
	// Systems
	// =======================================================================
	json::Array SystemRepository::listImages()
	{
		const QueryResult result = database_.query(
			"SELECT system_id, name, sysplex, region, status, version, created_at, updated_at "
			"FROM systems ORDER BY name");
		return result.toJson();
	}

	std::optional<json::Value> SystemRepository::findByName(const std::string &name)
	{
		const QueryResult result = database_.query(
			"SELECT system_id, name, sysplex, region, status, version, created_at, updated_at "
			"FROM systems WHERE name = ?",
			{text(str::upper(name))});
		if (result.rows.empty())
			return std::nullopt;
		return result.toJson().front();
	}

	std::int64_t SystemRepository::registerImage(const json::Value &image)
	{
		return insertOrFail(database_,
							"INSERT INTO systems (name, sysplex, region, status, version) VALUES (?, ?, ?, ?, ?)",
							{text(str::upper(image["name"].toString())),
							 text(str::upper(image["sysplex"].toString("SYSPLEX-A"))),
							 text(str::upper(image["region"].toString("DEFAULT"))),
							 text(str::upper(image["status"].toString("ACTIVE"))),
							 text(image["version"].toString("1.0.0"))},
							5, "system image");
	}

	bool SystemRepository::updateStatus(const std::string &name, const std::string &status)
	{
		return database_.executeUpdate(
				   "UPDATE systems SET status = ?, updated_at = datetime('now') WHERE name = ?",
				   {text(str::upper(status)), text(str::upper(name))}) > 0;
	}

	json::Array SystemRepository::parameters()
	{
		const QueryResult result = database_.query(
			"SELECT param_key, param_value, description, updated_at "
			"FROM system_parameters ORDER BY param_key");
		return result.toJson();
	}

	std::optional<std::string> SystemRepository::parameter(const std::string &key)
	{
		const QueryResult result = database_.query(
			"SELECT param_value FROM system_parameters WHERE param_key = ?", {text(key)});
		if (result.rows.empty() || result.rows.front().empty())
			return std::nullopt;
		return result.rows.front()[0].toString();
	}

	bool SystemRepository::setParameter(const std::string &key, const std::string &value,
										const std::string &description)
	{
		return database_.executeUpdate(
				   "INSERT INTO system_parameters (param_key, param_value, description, updated_at) "
				   "VALUES (?, ?, ?, datetime('now')) "
				   "ON CONFLICT(param_key) DO UPDATE SET param_value = excluded.param_value, "
				   "description = excluded.description, updated_at = datetime('now')",
				   {text(key), text(value), text(description)}) >= 0;
	}

} // namespace mf::db
