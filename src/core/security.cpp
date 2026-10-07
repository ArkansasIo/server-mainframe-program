#include "mf/core/security.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/ids.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace mf
{

	namespace
	{

		// -------------------------------------------------------------------
		// PBKDF2-HMAC-SHA256, implemented with a compact SHA-256 so the project
		// has no crypto dependency beyond the standard library.
		// -------------------------------------------------------------------
		struct Sha256
		{
			std::array<std::uint32_t, 8> state{
				0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
				0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
			std::uint64_t length = 0;
			std::array<unsigned char, 64> buffer{};
			std::size_t bufferLength = 0;

			static std::uint32_t rotr(std::uint32_t value, int bits)
			{
				return (value >> bits) | (value << (32 - bits));
			}

			void transform(const unsigned char *chunk)
			{
				static const std::uint32_t K[64] = {
					0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
					0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
					0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
					0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
					0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
					0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
					0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
					0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

				std::uint32_t w[64];
				for (int i = 0; i < 16; ++i)
				{
					w[i] = (static_cast<std::uint32_t>(chunk[i * 4]) << 24) | (static_cast<std::uint32_t>(chunk[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(chunk[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(chunk[i * 4 + 3]);
				}
				for (int i = 16; i < 64; ++i)
				{
					const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
					const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
					w[i] = w[i - 16] + s0 + w[i - 7] + s1;
				}

				std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
				std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

				for (int i = 0; i < 64; ++i)
				{
					const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
					const std::uint32_t ch = (e & f) ^ (~e & g);
					const std::uint32_t temp1 = h + s1 + ch + K[i] + w[i];
					const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
					const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
					const std::uint32_t temp2 = s0 + maj;

					h = g;
					g = f;
					f = e;
					e = d + temp1;
					d = c;
					c = b;
					b = a;
					a = temp1 + temp2;
				}

				state[0] += a;
				state[1] += b;
				state[2] += c;
				state[3] += d;
				state[4] += e;
				state[5] += f;
				state[6] += g;
				state[7] += h;
			}

			void update(const unsigned char *data, std::size_t size)
			{
				length += size;
				for (std::size_t i = 0; i < size; ++i)
				{
					buffer[bufferLength++] = data[i];
					if (bufferLength == 64)
					{
						transform(buffer.data());
						bufferLength = 0;
					}
				}
			}

			std::array<unsigned char, 32> finalize()
			{
				const std::uint64_t bitLength = length * 8;
				const unsigned char padding = 0x80;
				update(&padding, 1);
				const unsigned char zero = 0x00;
				while (bufferLength != 56)
					update(&zero, 1);

				unsigned char sizeBytes[8];
				for (int i = 0; i < 8; ++i)
				{
					sizeBytes[i] = static_cast<unsigned char>((bitLength >> (56 - i * 8)) & 0xFF);
				}
				update(sizeBytes, 8);

				std::array<unsigned char, 32> digest{};
				for (int i = 0; i < 8; ++i)
				{
					digest[i * 4] = static_cast<unsigned char>((state[i] >> 24) & 0xFF);
					digest[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xFF);
					digest[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xFF);
					digest[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xFF);
				}
				return digest;
			}
		};

		std::array<unsigned char, 32> sha256(std::string_view data)
		{
			Sha256 hasher;
			hasher.update(reinterpret_cast<const unsigned char *>(data.data()), data.size());
			return hasher.finalize();
		}

		std::array<unsigned char, 32> hmacSha256(std::string_view key, std::string_view message)
		{
			std::array<unsigned char, 64> block{};
			if (key.size() > block.size())
			{
				const auto digest = sha256(key);
				std::memcpy(block.data(), digest.data(), digest.size());
			}
			else
			{
				std::memcpy(block.data(), key.data(), key.size());
			}

			std::array<unsigned char, 64> innerPad{}, outerPad{};
			for (std::size_t i = 0; i < block.size(); ++i)
			{
				innerPad[i] = static_cast<unsigned char>(block[i] ^ 0x36);
				outerPad[i] = static_cast<unsigned char>(block[i] ^ 0x5c);
			}

			Sha256 inner;
			inner.update(innerPad.data(), innerPad.size());
			inner.update(reinterpret_cast<const unsigned char *>(message.data()), message.size());
			const auto innerDigest = inner.finalize();

			Sha256 outer;
			outer.update(outerPad.data(), outerPad.size());
			outer.update(innerDigest.data(), innerDigest.size());
			return outer.finalize();
		}

		std::string toHex(const unsigned char *data, std::size_t size)
		{
			static const char *digits = "0123456789abcdef";
			std::string out;
			out.reserve(size * 2);
			for (std::size_t i = 0; i < size; ++i)
			{
				out.push_back(digits[(data[i] >> 4) & 0x0F]);
				out.push_back(digits[data[i] & 0x0F]);
			}
			return out;
		}

		std::vector<unsigned char> fromHex(std::string_view hex)
		{
			std::vector<unsigned char> out;
			out.reserve(hex.size() / 2);
			auto value = [](char c) -> int
			{
				if (c >= '0' && c <= '9')
					return c - '0';
				if (c >= 'a' && c <= 'f')
					return c - 'a' + 10;
				if (c >= 'A' && c <= 'F')
					return c - 'A' + 10;
				return -1;
			};
			for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
			{
				const int hi = value(hex[i]);
				const int lo = value(hex[i + 1]);
				if (hi < 0 || lo < 0)
					break;
				out.push_back(static_cast<unsigned char>((hi << 4) | lo));
			}
			return out;
		}

		std::string pbkdf2(std::string_view password, const std::vector<unsigned char> &salt,
						   int iterations, std::size_t keyLength = 32)
		{
			std::string derived;
			derived.reserve(keyLength);
			const std::size_t blocks = (keyLength + 31) / 32;

			for (std::size_t block = 1; block <= blocks; ++block)
			{
				std::string saltBlock(salt.begin(), salt.end());
				saltBlock.push_back(static_cast<char>((block >> 24) & 0xFF));
				saltBlock.push_back(static_cast<char>((block >> 16) & 0xFF));
				saltBlock.push_back(static_cast<char>((block >> 8) & 0xFF));
				saltBlock.push_back(static_cast<char>(block & 0xFF));

				auto u = hmacSha256(password, saltBlock);
				auto result = u;
				for (int i = 1; i < iterations; ++i)
				{
					u = hmacSha256(password, std::string_view(
												 reinterpret_cast<const char *>(u.data()), u.size()));
					for (std::size_t j = 0; j < result.size(); ++j)
					{
						result[j] = static_cast<unsigned char>(result[j] ^ u[j]);
					}
				}
				derived.append(reinterpret_cast<const char *>(result.data()), result.size());
			}

			derived.resize(keyLength);
			return derived;
		}

		bool constantTimeEquals(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			unsigned char diff = 0;
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
			}
			return diff == 0;
		}

	} // namespace

	// -----------------------------------------------------------------------
	// Roles
	// -----------------------------------------------------------------------
	Role parseRole(std::string_view name)
	{
		const std::string lowered = str::lower(str::trim(name));
		if (lowered == "admin" || lowered == "administrator")
			return Role::Admin;
		if (lowered == "analyst")
			return Role::Analyst;
		if (lowered == "operator")
			return Role::Operator;
		return Role::Guest;
	}

	const char *roleName(Role role)
	{
		switch (role)
		{
		case Role::Admin:
			return "ADMIN";
		case Role::Analyst:
			return "ANALYST";
		case Role::Operator:
			return "OPERATOR";
		case Role::Guest:
			return "GUEST";
		}
		return "GUEST";
	}

	// -----------------------------------------------------------------------
	// password
	// -----------------------------------------------------------------------
	namespace password
	{

		std::string randomSaltHex(std::size_t bytes)
		{
			return ids::randomHex(static_cast<int>(bytes));
		}

		std::string hash(std::string_view plain, int iterations)
		{
			const std::string saltHex = randomSaltHex(16);
			const std::vector<unsigned char> salt = fromHex(saltHex);
			const std::string derived = pbkdf2(plain, salt, iterations);
			return "pbkdf2$" + std::to_string(iterations) + "$" + saltHex + "$" + toHex(reinterpret_cast<const unsigned char *>(derived.data()), derived.size());
		}

		bool isPlaceholder(std::string_view stored)
		{
			return str::startsWith(stored, "sha256:");
		}

		bool verify(std::string_view plain, std::string_view stored)
		{
			if (stored.empty())
				return false;

			if (str::startsWith(stored, "pbkdf2$"))
			{
				const auto parts = str::split(stored, '$', true);
				if (parts.size() != 4)
					return false;
				const int iterations = static_cast<int>(str::toInt(parts[1]).value_or(0));
				if (iterations <= 0)
					return false;
				const std::vector<unsigned char> salt = fromHex(parts[2]);
				if (salt.empty())
					return false;
				const std::string derived = pbkdf2(plain, salt, iterations);
				const std::string expected = toHex(
					reinterpret_cast<const unsigned char *>(derived.data()), derived.size());
				return constantTimeEquals(expected, parts[3]);
			}

			// Seeded placeholders can never be used to log on.
			if (isPlaceholder(stored))
				return false;

			// Development convenience: a plain-text stored value matches directly.
			return constantTimeEquals(plain, stored);
		}

		void validatePolicy(std::string_view plain, const json::Value &policy)
		{
			const std::size_t minLength = static_cast<std::size_t>(policy["minLength"].toInt(8));
			std::vector<std::string> problems;

			if (plain.size() < minLength)
			{
				problems.push_back("must be at least " + std::to_string(minLength) + " characters");
			}
			if (policy["requireDigit"].toBool(false) && plain.find_first_of("0123456789") == std::string_view::npos)
			{
				problems.push_back("must contain a digit");
			}
			if (policy["requireUpper"].toBool(false) && plain.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string_view::npos)
			{
				problems.push_back("must contain an uppercase letter");
			}
			if (policy["requireLower"].toBool(false) && plain.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string_view::npos)
			{
				problems.push_back("must contain a lowercase letter");
			}
			if (policy["requireSymbol"].toBool(false))
			{
				bool found = false;
				for (const char c : plain)
				{
					if (!std::isalnum(static_cast<unsigned char>(c)))
					{
						found = true;
						break;
					}
				}
				if (!found)
					problems.push_back("must contain a symbol");
			}

			if (!problems.empty())
			{
				throwValidation("Password " + str::join(problems, ", "));
			}
		}

	} // namespace password

	// -----------------------------------------------------------------------
	// Session
	// -----------------------------------------------------------------------
	json::Value Session::toJson() const
	{
		json::Value object = json::Value::object();
		object.set("session_id", json::Value(id));
		object.set("user_id", json::Value(static_cast<long long>(userId)));
		object.set("username", json::Value(username));
		object.set("role", json::Value(std::string(roleName(role))));
		object.set("terminal", json::Value(terminal));
		object.set("remote_address", json::Value(remoteAddress));
		object.set("created_at", json::Value(timeutil::toIso(
									 std::chrono::system_clock::time_point(std::chrono::milliseconds(createdAtMs)))));
		object.set("last_seen", json::Value(timeutil::toIso(
									std::chrono::system_clock::time_point(std::chrono::milliseconds(lastSeenMs)))));
		object.set("expires_at", json::Value(timeutil::toIso(
									 std::chrono::system_clock::time_point(std::chrono::milliseconds(expiresAtMs)))));
		object.set("commands_issued", json::Value(static_cast<long long>(commandsIssued)));
		return object;
	}

	// -----------------------------------------------------------------------
	// SessionManager
	// -----------------------------------------------------------------------
	SessionManager::SessionManager(int ttlMinutes, int maxPerUser)
		: ttlMinutes_(ttlMinutes > 0 ? ttlMinutes : 30), maxPerUser_(maxPerUser > 0 ? maxPerUser : 5)
	{
	}

	void SessionManager::configure(int ttlMinutes, int maxPerUser)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (ttlMinutes > 0)
			ttlMinutes_ = ttlMinutes;
		if (maxPerUser > 0)
			maxPerUser_ = maxPerUser;
	}

	Session SessionManager::create(std::int64_t userId, const std::string &username, Role role,
								   const std::string &terminal, const std::string &remoteAddress)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		purgeExpired();

		// Enforce the per-user session cap, dropping the oldest entries.
		std::vector<std::pair<std::int64_t, std::string>> existing;
		for (const auto &[id, session] : sessions_)
		{
			if (session.userId == userId)
				existing.emplace_back(session.lastSeenMs, id);
		}
		if (static_cast<int>(existing.size()) >= maxPerUser_)
		{
			std::sort(existing.begin(), existing.end());
			while (static_cast<int>(existing.size()) >= maxPerUser_ && !existing.empty())
			{
				sessions_.erase(existing.front().second);
				existing.erase(existing.begin());
			}
		}

		const std::int64_t now = timeutil::epochMs();
		Session session;
		session.id = ids::session();
		session.userId = userId;
		session.username = username;
		session.role = role;
		session.terminal = terminal;
		session.remoteAddress = remoteAddress;
		session.createdAtMs = now;
		session.lastSeenMs = now;
		session.expiresAtMs = now + static_cast<std::int64_t>(ttlMinutes_) * 60000;

		sessions_[session.id] = session;
		return session;
	}

	std::optional<Session> SessionManager::get(const std::string &sessionId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = sessions_.find(sessionId);
		if (it == sessions_.end())
			return std::nullopt;
		if (it->second.expired(timeutil::epochMs()))
		{
			sessions_.erase(it);
			return std::nullopt;
		}
		return it->second;
	}

	bool SessionManager::touch(const std::string &sessionId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = sessions_.find(sessionId);
		if (it == sessions_.end())
			return false;
		const std::int64_t now = timeutil::epochMs();
		if (it->second.expired(now))
		{
			sessions_.erase(it);
			return false;
		}
		it->second.lastSeenMs = now;
		it->second.expiresAtMs = now + static_cast<std::int64_t>(ttlMinutes_) * 60000;
		it->second.commandsIssued += 1;
		return true;
	}

	bool SessionManager::destroy(const std::string &sessionId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return sessions_.erase(sessionId) > 0;
	}

	int SessionManager::destroyForUser(std::int64_t userId)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		int removed = 0;
		for (auto it = sessions_.begin(); it != sessions_.end();)
		{
			if (it->second.userId == userId)
			{
				it = sessions_.erase(it);
				++removed;
			}
			else
			{
				++it;
			}
		}
		return removed;
	}

	int SessionManager::purgeExpired()
	{
		const std::int64_t now = timeutil::epochMs();
		int removed = 0;
		for (auto it = sessions_.begin(); it != sessions_.end();)
		{
			if (it->second.expired(now))
			{
				it = sessions_.erase(it);
				++removed;
			}
			else
			{
				++it;
			}
		}
		return removed;
	}

	std::vector<Session> SessionManager::listActive()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		purgeExpired();
		std::vector<Session> out;
		out.reserve(sessions_.size());
		for (const auto &[_, session] : sessions_)
			out.push_back(session);
		std::sort(out.begin(), out.end(), [](const Session &a, const Session &b)
				  { return a.createdAtMs > b.createdAtMs; });
		return out;
	}

	std::size_t SessionManager::size()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return sessions_.size();
	}

	// -----------------------------------------------------------------------
	// SecurityService
	// -----------------------------------------------------------------------
	SecurityService::SecurityService(db::UserRepository &users, const json::Value &securityConfig)
		: users_(users)
	{
		const auto &sessionConfig = securityConfig["session"];
		sessions_.configure(
			static_cast<int>(sessionConfig["ttlMinutes"].toInt(30)),
			static_cast<int>(sessionConfig["maxPerUser"].toInt(5)));

		passwordPolicy_ = securityConfig["passwordPolicy"];
		if (!passwordPolicy_.isObject())
		{
			passwordPolicy_ = json::Value::object();
			passwordPolicy_.set("minLength", json::Value(8));
		}

			lockThreshold_ = static_cast<int>(securityConfig["lockThreshold"].toInt(5));
			if (lockThreshold_ <= 0) lockThreshold_ = 5;

			const json::Value& keys = securityConfig["apiKeys"];
			if (keys.isArray()) {
				for (const auto& key : keys.items()) {
					if (key.isString() && !key.asString().empty()) apiKeys_.insert(key.asString());
				}
			}
		}

	bool SecurityService::validateApiKey(std::string_view key) const
	{
		if (apiKeys_.empty())
			return true; // no keys configured -> open
		return apiKeys_.count(std::string(key)) > 0;
	}

	bool SecurityService::hasRole(Role actual, Role required)
	{
		return static_cast<int>(actual) >= static_cast<int>(required);
	}

	bool SecurityService::hasRoleName(std::string_view actual, std::string_view required)
	{
		return hasRole(parseRole(actual), parseRole(required));
	}

	void SecurityService::hashUserPassword(std::int64_t userId, const std::string &plain)
	{
		password::validatePolicy(plain, passwordPolicy_);
		users_.setPasswordHash(userId, password::hash(plain));
	}

	SecurityService::LogonResult SecurityService::logon(const std::string &username,
														const std::string &plainPassword,
														const std::string &terminal,
														const std::string &remoteAddress)
	{
		LogonResult result;

		const std::string normalized = str::upper(str::trim(username));
		if (normalized.empty())
		{
			result.reason = "USERID REQUIRED";
			return result;
		}

		const auto user = users_.findByUsername(normalized);
		if (!user)
		{
			result.reason = "USERID NOT RECOGNISED";
			return result;
		}

		const std::string status = (*user)["status"].toString("ACTIVE");
		if (status == "DISABLED")
		{
			result.reason = "USERID REVOKED";
			return result;
		}
		if (status == "LOCKED")
		{
			result.reason = "USERID LOCKED - CONTACT SECURITY ADMINISTRATOR";
			return result;
		}

		const std::string stored = (*user)["password_hash"].toString("");
		if (password::isPlaceholder(stored))
		{
			result.reason = "PASSWORD NOT SET FOR THIS USERID";
			return result;
		}

		if (!password::verify(plainPassword, stored))
		{
			users_.recordFailedLogon((*user)["user_id"].toInt(0), lockThreshold_);
			result.reason = "PASSWORD INCORRECT";
			return result;
		}

		const std::int64_t userId = (*user)["user_id"].toInt(0);
		users_.recordLogon(userId);

		result.success = true;
		result.session = sessions_.create(userId, normalized,
										  parseRole((*user)["role"].toString("GUEST")), terminal, remoteAddress);
		result.user = *user;
		result.user.erase("password_hash");
		return result;
	}

	bool SecurityService::logoff(const std::string &sessionId)
	{
		return sessions_.destroy(sessionId);
	}

} // namespace mf
