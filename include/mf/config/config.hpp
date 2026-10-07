// ---------------------------------------------------------------------------
// Layered configuration.
//
// Resolution order (later wins):
//   1. config/default.json
//   2. config/local.json            (or an explicit --config <path>)
//   3. MF_* environment variables
//   4. command line flags
//
// Paths referencing the filesystem (database file, storage, logs) are made
// absolute against the project root during resolve().
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mf/util/json.hpp"

namespace mf::config
{

	struct CommandLine
	{
		std::optional<std::string> configPath;
		json::Value overrides = json::Value::object();
		bool tcpOnly = false;
		bool httpOnly = false;
	};

	// -- parsing helpers -------------------------------------------------------
	CommandLine parseCommandLine(const std::vector<std::string> &argv);

	// Environment variable -> dotted config path mapping.
	const std::map<std::string, std::string> &environmentMap();
	json::Value environmentOverrides(const std::map<std::string, std::string> &env);

	// Coerce an environment string into a JSON scalar. "8" -> 8, "true" ->
	// true, "a,b" -> [a,b], "x" -> "x".
	json::Value coerceScalar(const std::string &raw);

	// -- resolution ------------------------------------------------------------
	struct LoadOptions
	{
		std::optional<std::string> configPath;
		std::vector<std::string> argv;
		std::optional<std::map<std::string, std::string>> environment;
		json::Value overrides = json::Value::object();
		std::filesystem::path projectRoot; // defaults to the executable's parent/parent
	};

	struct Resolved
	{
		json::Value values = json::Value::object();
		std::vector<std::string> warnings;
		std::string sourceFile;
		std::filesystem::path projectRoot;
		bool tcpOnly = false;
		bool httpOnly = false;
	};

	// Throws mf::Error(ErrorCode::Configuration) when the result is invalid.
	Resolved load(const LoadOptions &options);

	// Validate an already merged tree. Collects fatal problems into `errors`
	// and non-fatal ones into `warnings`.
	struct ValidationResult
	{
		std::vector<std::string> errors;
		std::vector<std::string> warnings;
		bool ok() const { return errors.empty(); }
	};

	ValidationResult validate(const json::Value &config);

	// Mask secret looking fields before display (-show-config).
	json::Value maskSecrets(const json::Value &config);

	std::filesystem::path projectRootFromExecutable();

} // namespace mf::config
