// ---------------------------------------------------------------------------
// Repositories: one thin, typed accessor per table. Every method maps rows to
// JSON objects so the HTTP and terminal layers never touch SQL directly.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mf/db/sqlite.hpp"

namespace mf::db
{

	struct Page
	{
		std::int64_t limit = 50;
		std::int64_t offset = 0;
		std::optional<std::string> orderBy;
		bool descending = false;
	};

	// Shared paging helpers.
	std::string buildOrderClause(const std::optional<std::string> &column,
								 const std::vector<std::string> &allowed,
								 bool descending,
								 const std::string &fallback);

	class UserRepository
	{
	public:
		explicit UserRepository(Database &database) : database_(database) {}

		json::Array list(const Page &page = {});
		std::int64_t count();
		std::optional<json::Value> findById(std::int64_t userId);
		std::optional<json::Value> findByUsername(const std::string &username);
		json::Array findByRole(const std::string &role);

		std::int64_t create(const json::Value &user);
		bool update(std::int64_t userId, const json::Value &fields);
		bool remove(std::int64_t userId);

		bool recordLogon(std::int64_t userId);
		bool recordFailedLogon(std::int64_t userId, int lockThreshold);
		bool resetFailedLogons(std::int64_t userId);
		bool setStatus(std::int64_t userId, const std::string &status);
		bool setPasswordHash(std::int64_t userId, const std::string &hash);

		std::int64_t countByRole(const std::string &role);

	private:
		Database &database_;
	};

	class DatasetRepository
	{
	public:
		explicit DatasetRepository(Database &database) : database_(database) {}

		json::Array list(const Page &page = {});
		json::Array search(const std::string &pattern, const Page &page = {});
		std::int64_t count();
		std::optional<json::Value> findById(std::int64_t datasetId);
		std::optional<json::Value> findByName(const std::string &name);
		bool exists(const std::string &name);

		std::int64_t create(const json::Value &dataset);
		bool update(std::int64_t datasetId, const json::Value &fields);
		bool remove(std::int64_t datasetId);
		bool setStatus(std::int64_t datasetId, const std::string &status);
		bool migrateToVolume(std::int64_t datasetId, const std::string &volume);
		bool updateCounters(std::int64_t datasetId);
		bool transferOwner(std::int64_t datasetId, std::int64_t newOwnerId);

		std::int64_t totalRecords();
		std::int64_t totalBytes();
		json::Array byVolume();
		json::Array byOwner(std::int64_t ownerId);

	private:
		Database &database_;
	};

	class RecordRepository
	{
	public:
		explicit RecordRepository(Database &database) : database_(database) {}

		json::Array list(std::int64_t datasetId, std::int64_t limit = 500, std::int64_t offset = 0);
		std::int64_t count(std::int64_t datasetId);
		std::optional<json::Value> findById(std::int64_t recordId);

		// Append one record; sequence is derived from the current maximum.
		std::int64_t append(std::int64_t datasetId, const std::string &payload);
		std::int64_t appendMany(std::int64_t datasetId, const std::vector<std::string> &payloads);
		std::int64_t replaceAll(std::int64_t datasetId, const std::vector<std::string> &payloads);

		bool remove(std::int64_t recordId);
		std::int64_t removeAll(std::int64_t datasetId);
		std::int64_t nextSequence(std::int64_t datasetId);

		// Stream payloads as raw text (one record per line).
		std::string exportPayload(std::int64_t datasetId);

	private:
		Database &database_;
	};

	class JobRepository
	{
	public:
		explicit JobRepository(Database &database) : database_(database) {}

		json::Array list(const Page &page = {});
		json::Array byStatus(const std::string &status, std::int64_t limit = 100);
		json::Array queue();
		std::int64_t count();
		std::optional<json::Value> findById(std::int64_t jobId);
		std::int64_t countByStatus(const std::string &status);

		std::int64_t create(const json::Value &job);
		bool markRunning(std::int64_t jobId);
		bool markComplete(std::int64_t jobId, int returnCode, std::int64_t cpuMs, const std::string &output);
		bool markAbend(std::int64_t jobId, int returnCode, const std::string &output);
		bool cancel(std::int64_t jobId);
		bool hold(std::int64_t jobId);
		bool release(std::int64_t jobId);
		bool purge(const std::string &status, int olderThanDays);

		json::Value summary();

	private:
		Database &database_;
	};

	class TransactionRepository
	{
	public:
		explicit TransactionRepository(Database &database) : database_(database) {}

		json::Array list(const Page &page = {});
		std::int64_t count();
		std::optional<json::Value> findById(std::int64_t txnId);
		json::Array byCode(const std::string &txnCode, std::int64_t limit = 100);
		json::Array recentFailures(std::int64_t limit = 50);

		std::int64_t create(const json::Value &transaction);
		bool record(const std::string &txnCode, const std::string &terminal,
					std::int64_t userId, const std::string &status,
					std::int64_t rowsRead, std::int64_t rowsWritten,
					std::int64_t elapsedMs, const std::string &detail);
		json::Value activityReport();

	private:
		Database &database_;
	};

	class AuditRepository
	{
	public:
		explicit AuditRepository(Database &database) : database_(database) {}

		json::Array list(const Page &page = {});
		json::Array recent(std::int64_t limit = 100);
		json::Array byActor(const std::string &actor, std::int64_t limit = 100);
		json::Array search(const std::string &text, std::int64_t limit = 100);
		std::int64_t count();

		std::int64_t write(const std::string &eventType, const std::string &severity,
						   const std::string &actor, const std::string &resource,
						   const std::string &message, const json::Value &metadata);

		std::int64_t purgeOlderThan(int days);
		json::Value summaryByDay(int days = 30);
		json::Value summaryBySeverity();

	private:
		Database &database_;
	};

	class VolumeRepository
	{
	public:
		explicit VolumeRepository(Database &database) : database_(database) {}

		json::Array list();
		std::optional<json::Value> findBySerial(const std::string &serial);
		std::int64_t create(const json::Value &volume);
		bool updateUsage(const std::string &serial, std::int64_t usedMb);
		json::Array utilization();

	private:
		Database &database_;
	};

	class SystemRepository
	{
	public:
		explicit SystemRepository(Database &database) : database_(database) {}

		json::Array listImages();
		std::optional<json::Value> findByName(const std::string &name);
		std::int64_t registerImage(const json::Value &image);
		bool updateStatus(const std::string &name, const std::string &status);

		json::Array parameters();
		std::optional<std::string> parameter(const std::string &key);
		bool setParameter(const std::string &key, const std::string &value,
						  const std::string &description = "");

	private:
		Database &database_;
	};

	// Aggregate facade so callers construct one object.
	struct Repositories
	{
		explicit Repositories(Database &database)
			: users(database), datasets(database), records(database), jobs(database), transactions(database), audit(database), volumes(database), systems(database)
		{
		}

		UserRepository users;
		DatasetRepository datasets;
		RecordRepository records;
		JobRepository jobs;
		TransactionRepository transactions;
		AuditRepository audit;
		VolumeRepository volumes;
		SystemRepository systems;
	};

} // namespace mf::db
