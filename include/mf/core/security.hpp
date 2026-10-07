// ---------------------------------------------------------------------------
// Security services: password hashing (scrypt via a deterministic PBKDF2
// fallback), session management (RACF-style), authorisation checks and API key
// validation.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "mf/db/repository.hpp"
#include "mf/util/json.hpp"

namespace mf
{

	enum class Role
	{
		Guest = 0,
		Operator = 1,
		Analyst = 2,
		Admin = 3
	};

	Role parseRole(std::string_view name);
	const char *roleName(Role role);

	// -----------------------------------------------------------------------
	// Password hashing
	// -----------------------------------------------------------------------
	namespace password
	{

		// Format: pbkdf2$<iterations>$<saltHex>$<hashHex>
		std::string hash(std::string_view plain, int iterations = 120000);

		// Constant-time verification. Understands the legacy "sha256:seed"
		// placeholders from sql/seed.sql, which are deliberately unusable.
		bool verify(std::string_view plain, std::string_view stored);

		// True when the stored value is a seeded placeholder that can never match.
		bool isPlaceholder(std::string_view stored);

		std::string randomSaltHex(std::size_t bytes = 16);

		void validatePolicy(std::string_view plain, const json::Value &policy);

	} // namespace password

	// -----------------------------------------------------------------------
	// Sessions
	// -----------------------------------------------------------------------
	struct Session
	{
		std::string id;
		std::int64_t userId = 0;
		std::string username;
		Role role = Role::Guest;
		std::string terminal; // "HTTP", "L3270", ...
		std::string remoteAddress;
		std::int64_t createdAtMs = 0;
		std::int64_t lastSeenMs = 0;
		std::int64_t expiresAtMs = 0;
		std::int64_t commandsIssued = 0;

		bool expired(std::int64_t nowMs) const { return nowMs >= expiresAtMs; }
		double minutesRemaining(std::int64_t nowMs) const
		{
			return static_cast<double>(expiresAtMs - nowMs) / 60000.0;
		}
		json::Value toJson() const;
	};

	class SessionManager
	{
	public:
		SessionManager(int ttlMinutes = 30, int maxPerUser = 5);

		void configure(int ttlMinutes, int maxPerUser);

		Session create(std::int64_t userId, const std::string &username, Role role,
					   const std::string &terminal, const std::string &remoteAddress);

		std::optional<Session> get(const std::string &sessionId);
		bool touch(const std::string &sessionId);
		bool destroy(const std::string &sessionId);
		int destroyForUser(std::int64_t userId);

		// Remove expired entries; returns how many were purged.
		int purgeExpired();

		std::vector<Session> listActive();
		std::size_t size();

		int ttlMinutes() const { return ttlMinutes_; }

	private:
		mutable std::mutex mutex_;
		std::map<std::string, Session> sessions_;
		int ttlMinutes_;
		int maxPerUser_;
	};

	// -----------------------------------------------------------------------
	// Authentication / authorisation facade
	// -----------------------------------------------------------------------
	class SecurityService
	{
	public:
		SecurityService(db::UserRepository &users, const json::Value &securityConfig);

		struct LogonResult
		{
			bool success = false;
			std::string reason;
			Session session;
			json::Value user = json::Value::object();
		};

		// Interactive logon used by the terminal gateway (and /api/auth/logon).
		LogonResult logon(const std::string &username, const std::string &password,
						  const std::string &terminal, const std::string &remoteAddress);

		bool logoff(const std::string &sessionId);

		// API key check for the HTTP control plane.
		bool validateApiKey(std::string_view key) const;

		// Role comparison helper: does `actual` satisfy `required`?
		static bool hasRole(Role actual, Role required);
		static bool hasRoleName(std::string_view actual, std::string_view required);

		SessionManager &sessions() { return sessions_; }
		const SessionManager &sessions() const { return sessions_; }

		void hashUserPassword(std::int64_t userId, const std::string &plain);

		int lockThreshold() const { return lockThreshold_; }
		const json::Value &passwordPolicy() const { return passwordPolicy_; }

	private:
		db::UserRepository &users_;
		SessionManager sessions_;
		std::set<std::string> apiKeys_;
		json::Value passwordPolicy_;
		int lockThreshold_ = 5;
	};

} // namespace mf
