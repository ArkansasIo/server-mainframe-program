// ---------------------------------------------------------------------------
// Audit trail (SMF-style). Writes structured events to audit_log and mirrors
// them into the logger so both the database and the log file tell the story.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

#include "mf/core/logger.hpp"
#include "mf/db/repository.hpp"
#include "mf/util/json.hpp"

namespace mf {

	enum class AuditSeverity { Debug, Info, Warn, Error, Critical };

	const char* auditSeverityName(AuditSeverity severity);
	AuditSeverity parseAuditSeverity(std::string_view name, AuditSeverity fallback = AuditSeverity::Info);

	// Event type constants, matching the CHECK-less TEXT column in schema.sql.
	namespace AuditEvent {
		inline constexpr const char* Logon = "LOGON";
		inline constexpr const char* Logoff = "LOGOFF";
		inline constexpr const char* Command = "COMMAND";
		inline constexpr const char* Database = "DB";
		inline constexpr const char* Config = "CONFIG";
		inline constexpr const char* Job = "JOB";
		inline constexpr const char* Dataset = "DATASET";
		inline constexpr const char* Transfer = "TRANSFER";
		inline constexpr const char* Security = "SECURITY";
		inline constexpr const char* Http = "HTTP";
	} // namespace AuditEvent

	class AuditService {
	public:
		AuditService(db::AuditRepository& repository, Logger& logger, bool enabled = true);

		void setEnabled(bool enabled) { enabled_ = enabled; }
		bool enabled() const { return enabled_; }

		// Core writer. decrements and metadata are optional.
		std::int64_t write(std::string_view eventType, AuditSeverity severity,
			std::string_view actor, std::string_view resource,
			std::string_view message, const json::Value& metadata = json::Value::object());

		// Convenience wrappers.
		std::int64_t logon(std::string_view actor, std::string_view terminal, bool accepted,
			std::string_view detail = "");
		std::int64_t logoff(std::string_view actor, std::string_view terminal);
		std::int64_t command(std::string_view actor, std::string_view command,
			std::string_view terminal, bool accepted, std::string_view detail = "");
		std::int64_t database(std::string_view operation, std::string_view resource,
			std::string_view message, const json::Value& metadata = json::Value::object());
		std::int64_t config(std::string_view actor, std::string_view message,
			const json::Value& metadata = json::Value::object());
		std::int64_t security(std::string_view actor, std::string_view message,
			AuditSeverity severity = AuditSeverity::Warn,
			const json::Value& metadata = json::Value::object());
		std::int64_t jobEvent(std::string_view actor, std::string_view jobName,
			std::string_view message, AuditSeverity severity = AuditSeverity::Info);
		std::int64_t datasetEvent(std::string_view actor, std::string_view datasetName,
			std::string_view message, AuditSeverity severity = AuditSeverity::Info);
		std::int64_t transfer(std::string_view actor, std::string_view datasetName,
			std::string_view direction, std::int64_t records, std::int64_t bytes);

		int purgeOlderThan(int days);
		json::Value summaryBySeverity();
		json::Array recent(std::int64_t limit = 100);

		// True when logon/command events should be recorded per configuration.
		void configureFromConfig(const json::Value& securityConfig);

	private:
		db::AuditRepository& repository_;
		Logger& logger_;
		bool enabled_ = true;
		bool logLogons_ = true;
		bool logCommands_ = true;
	};

} // namespace mf
