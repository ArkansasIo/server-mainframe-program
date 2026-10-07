// ---------------------------------------------------------------------------
// Audit trail (SMF-style). Writes structured events to audit_log and mirrors
// them into the logger so both the database and the log file tell the story.
// ---------------------------------------------------------------------------
#include "mf/core/audit.hpp"

#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

namespace mf {

	const char* auditSeverityName(AuditSeverity severity) {
		switch (severity) {
		case AuditSeverity::Debug: return "DEBUG";
		case AuditSeverity::Info: return "INFO";
		case AuditSeverity::Warn: return "WARN";
		case AuditSeverity::Error: return "ERROR";
		case AuditSeverity::Critical: return "CRITICAL";
		}
		return "INFO";
	}

	AuditSeverity parseAuditSeverity(std::string_view name, AuditSeverity fallback) {
		const std::string lowered = str::lower(str::trim(name));
		if (lowered == "debug") return AuditSeverity::Debug;
		if (lowered == "info") return AuditSeverity::Info;
		if (lowered == "warn" || lowered == "warning") return AuditSeverity::Warn;
		if (lowered == "error") return AuditSeverity::Error;
		if (lowered == "critical" || lowered == "crit") return AuditSeverity::Critical;
		return fallback;
	}

	namespace {
		LogLevel toLogLevel(AuditSeverity severity) {
			switch (severity) {
			case AuditSeverity::Debug: return LogLevel::Debug;
			case AuditSeverity::Info: return LogLevel::Info;
			case AuditSeverity::Warn: return LogLevel::Warn;
			case AuditSeverity::Error:
			case AuditSeverity::Critical: return LogLevel::Error;
			}
			return LogLevel::Info;
		}
	} // namespace

	AuditService::AuditService(db::AuditRepository& repository, Logger& logger, bool enabled)
		: repository_(repository), logger_(logger), enabled_(enabled) {
	}

	void AuditService::configureFromConfig(const json::Value& securityConfig) {
		const auto& audit = securityConfig["audit"];
		if (audit.isObject()) {
			enabled_ = audit["enabled"].toBool(enabled_);
			logLogons_ = audit["logLogons"].toBool(true);
			logCommands_ = audit["logCommands"].toBool(true);
		}
	}

	std::int64_t AuditService::write(std::string_view eventType, AuditSeverity severity,
		std::string_view actor, std::string_view resource,
		std::string_view message, const json::Value& metadata) {
		// Mirror to the logger regardless, so the log file stays complete.
		Logger& log = logger_;
		log.log(toLogLevel(severity),
			std::string("[audit] ") + std::string(eventType) + " " + std::string(message),
			metadata);

		if (!enabled_) return 0;

		try {
			return repository_.write(std::string(eventType), auditSeverityName(severity),
				std::string(actor), std::string(resource), std::string(message), metadata);
		}
		catch (...) {
			// Auditing must never break the request path.
			return 0;
		}
	}

	std::int64_t AuditService::logon(std::string_view actor, std::string_view terminal,
		bool accepted, std::string_view detail) {
		if (!logLogons_) return 0;
		json::Value metadata = json::Value::object();
		metadata.set("terminal", json::Value(std::string(terminal)));
		metadata.set("accepted", json::Value(accepted));
		if (!detail.empty()) metadata.set("detail", json::Value(std::string(detail)));

		return write(AuditEvent::Logon,
			accepted ? AuditSeverity::Info : AuditSeverity::Warn,
			accepted ? actor : "UNKNOWN", "SYSTEM",
			accepted ? "Operator logon accepted" : "Logon rejected - " + std::string(detail),
			metadata);
	}

	std::int64_t AuditService::logoff(std::string_view actor, std::string_view terminal) {
		if (!logLogons_) return 0;
		json::Value metadata = json::Value::object();
		metadata.set("terminal", json::Value(std::string(terminal)));
		return write(AuditEvent::Logoff, AuditSeverity::Info, actor, "SYSTEM",
			"Operator logoff", metadata);
	}

	std::int64_t AuditService::command(std::string_view actor, std::string_view commandText,
		std::string_view terminal, bool accepted, std::string_view detail) {
		if (!logCommands_) return 0;
		json::Value metadata = json::Value::object();
		metadata.set("terminal", json::Value(std::string(terminal)));
		metadata.set("command", json::Value(std::string(commandText)));
		metadata.set("accepted", json::Value(accepted));
		if (!detail.empty()) metadata.set("detail", json::Value(std::string(detail)));

		return write(AuditEvent::Command,
			accepted ? AuditSeverity::Info : AuditSeverity::Warn,
			actor, std::string(terminal), std::string(commandText), metadata);
	}

	std::int64_t AuditService::database(std::string_view operation, std::string_view resource,
		std::string_view message, const json::Value& metadata) {
		json::Value payload = metadata.isObject() ? metadata : json::Value::object();
		payload.set("operation", json::Value(std::string(operation)));
		return write(AuditEvent::Database, AuditSeverity::Info, "SYSTEM", resource, message, payload);
	}

	std::int64_t AuditService::config(std::string_view actor, std::string_view message,
		const json::Value& metadata) {
		return write(AuditEvent::Config, AuditSeverity::Info, actor, "CONFIG", message, metadata);
	}

	std::int64_t AuditService::security(std::string_view actor, std::string_view message,
		AuditSeverity severity, const json::Value& metadata) {
		return write(AuditEvent::Security, severity, actor, "SECURITY", message, metadata);
	}

	std::int64_t AuditService::jobEvent(std::string_view actor, std::string_view jobName,
		std::string_view message, AuditSeverity severity) {
		return write(AuditEvent::Job, severity, actor, jobName, message);
	}

	std::int64_t AuditService::datasetEvent(std::string_view actor, std::string_view datasetName,
		std::string_view message, AuditSeverity severity) {
		return write(AuditEvent::Dataset, severity, actor, datasetName, message);
	}

	std::int64_t AuditService::transfer(std::string_view actor, std::string_view datasetName,
		std::string_view direction, std::int64_t records, std::int64_t bytes) {
		json::Value metadata = json::Value::object();
		metadata.set("direction", json::Value(std::string(direction)));
		metadata.set("records", json::Value(static_cast<long long>(records)));
		metadata.set("bytes", json::Value(static_cast<long long>(bytes)));
		return write(AuditEvent::Transfer, AuditSeverity::Info, actor, datasetName,
			std::string(direction) + " " + std::to_string(records) + " record(s)", metadata);
	}

	int AuditService::purgeOlderThan(int days) {
		try {
			const std::int64_t removed = repository_.purgeOlderThan(days);
			logger_.info("purged old audit records", json::Value(static_cast<long long>(removed)));
			return static_cast<int>(removed);
		}
		catch (const Error& ex) {
			logger_.error(std::string("audit purge failed: ") + ex.what());
			return 0;
		}
	}

	json::Value AuditService::summaryBySeverity() {
		return repository_.summaryBySeverity();
	}

	json::Array AuditService::recent(std::int64_t limit) {
		return repository_.recent(limit);
	}

} // namespace mf
