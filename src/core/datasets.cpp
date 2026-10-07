#include "mf/core/datasets.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/time.hpp"

#include <algorithm>
#include <numeric>
#include <set>

namespace mf
{

	namespace
	{
		const std::set<std::string> VALID_DSORG = {"PS", "PO", "PDS", "VSAM"};
		const std::set<std::string> VALID_RECFM = {"F", "FB", "VB", "VBS", "U"};
		const std::set<std::string> VALID_STATUS = {"AVAILABLE", "MIGRATED", "DELETED"};

		// Dataset name qualifiers are 1-8 characters, A-Z 0-9 $ # @, at most 22
		// qualifiers. The whole name is capped at 44 characters like z/OS.
		bool validQualifier(const std::string &qualifier)
		{
			if (qualifier.empty() || qualifier.size() > 8)
				return false;
			for (const char c : qualifier)
			{
				const bool upper = c >= 'A' && c <= 'Z';
				const bool digit = c >= '0' && c <= '9';
				const bool special = c == '$' || c == '#' || c == '@' || c == '-';
				if (!upper && !digit && !special)
					return false;
			}
			return qualifier.front() != '-' && qualifier.back() != '-';
		}
	} // namespace

	std::vector<std::string> splitQualifiers(const std::string &name)
	{
		return str::split(str::upper(name), '.');
	}

	std::string datasetPrefix(const std::string &name)
	{
		const auto qualifiers = splitQualifiers(name);
		if (qualifiers.size() < 2)
			return qualifiers.empty() ? "" : qualifiers.front();
		return qualifiers[0] + "." + qualifiers[1];
	}

	bool isValidDatasetName(const std::string &name)
	{
		const std::string upper = str::upper(str::trim(name));
		if (upper.empty() || upper.size() > 44)
			return false;
		if (upper.front() == '.' || upper.back() == '.')
			return false;
		const auto qualifiers = str::split(upper, '.');
		if (qualifiers.size() > 22)
			return false;
		for (const auto &qualifier : qualifiers)
		{
			if (!validQualifier(qualifier))
				return false;
		}
		return true;
	}

	bool isValidRecordFormat(const std::string &recfm)
	{
		return VALID_RECFM.count(str::upper(recfm)) > 0;
	}

	bool isValidOrganization(const std::string &dsorg)
	{
		return VALID_DSORG.count(str::upper(dsorg)) > 0;
	}

	std::int64_t estimateSpace(std::int64_t recordCount, std::int64_t lrecl)
	{
		// Each record carries a 4 byte Record Descriptor Word in fixed/variable
		// blocked formats, matching how bytes_used is charged in the schema.
		return std::max<std::int64_t>(0, recordCount) * (std::max<std::int64_t>(0, lrecl) + 4);
	}

	DatasetService::DatasetService(db::Repositories &repositories, AuditService &audit, Logger &logger)
		: repositories_(repositories), audit_(audit), logger_(logger)
	{
	}

	DatasetAttributes DatasetService::validate(const DatasetAttributes &attributes)
	{
		DatasetAttributes out = attributes;
		out.name = str::upper(str::trim(out.name));
		out.dsorg = str::upper(str::trim(out.dsorg));
		out.recfm = str::upper(str::trim(out.recfm));
		out.volume = str::upper(str::trim(out.volume));

		if (!isValidDatasetName(out.name))
		{
			throwValidation("'" + attributes.name + "' is not a valid dataset name "
													"(qualifiers of 1-8 characters, maximum 44 characters total)");
		}
		if (!isValidOrganization(out.dsorg))
		{
			throwValidation("DSORG must be one of PS, PO, PDS, VSAM (got " + out.dsorg + ")");
		}
		if (!isValidRecordFormat(out.recfm))
		{
			throwValidation("RECFM must be one of F, FB, VB, VBS, U (got " + out.recfm + ")");
		}
		if (out.lrecl < 1 || out.lrecl > 32760)
		{
			throwValidation("LRECL must be between 1 and 32760 (got " + std::to_string(out.lrecl) + ")");
		}
		if (out.blksize < 1)
		{
			throwValidation("BLKSIZE must be greater than zero");
		}
		if (out.volume.empty())
			out.volume = "MFVOL1";
		return out;
	}

	json::Value DatasetService::create(const DatasetAttributes &attributes, const std::string &actor)
	{
		const DatasetAttributes valid = validate(attributes);

		if (repositories_.datasets.exists(valid.name))
		{
			throwConflict("dataset " + valid.name + " already exists in the catalog");
		}

		json::Value payload = json::Value::object();
		payload.set("name", json::Value(valid.name));
		payload.set("dsorg", json::Value(valid.dsorg));
		payload.set("recfm", json::Value(valid.recfm));
		payload.set("lrecl", json::Value(static_cast<long long>(valid.lrecl)));
		payload.set("blksize", json::Value(static_cast<long long>(valid.blksize)));
		payload.set("volume", json::Value(valid.volume));
		payload.set("owner_user_id", valid.ownerUserId > 0
										 ? json::Value(static_cast<long long>(valid.ownerUserId))
										 : json::Value());

		const std::int64_t datasetId = repositories_.datasets.create(payload);
		if (datasetId <= 0)
			throwDatabase("failed to allocate dataset " + valid.name);

		refreshVolumeUsage(valid.volume);

		audit_.datasetEvent(actor.empty() ? "SYSTEM" : actor, valid.name,
							"Dataset allocated on volume " + valid.volume + " (" + valid.dsorg + "/" + valid.recfm + ")");

		logger_.info("dataset allocated", json::Value::object());
		const auto created = repositories_.datasets.findById(datasetId);
		return created ? *created : json::Value::object();
	}

	bool DatasetService::allocate(const std::string &name, const DatasetAttributes &attributes,
								  const std::string &actor)
	{
		DatasetAttributes copy = attributes;
		copy.name = name;
		create(copy, actor);
		return true;
	}

	bool DatasetService::remove(std::int64_t datasetId, bool purgeRecords, const std::string &actor)
	{
		const auto dataset = repositories_.datasets.findById(datasetId);
		if (!dataset)
			throwNotFound("dataset " + std::to_string(datasetId));

		const std::string name = (*dataset)["name"].toString();
		const std::string volume = (*dataset)["volume"].toString();

		if (purgeRecords)
		{
			repositories_.datasets.remove(datasetId); // cascades to dataset_records
		}
		else
		{
			// Soft delete: keep the catalog entry but mark it deleted.
			repositories_.datasets.setStatus(datasetId, "DELETED");
		}

		refreshVolumeUsage(volume);

		audit_.datasetEvent(actor.empty() ? "SYSTEM" : actor, name,
							purgeRecords ? "Dataset deleted (records purged)" : "Dataset marked DELETED",
							AuditSeverity::Warn);
		return true;
	}

	bool DatasetService::rename(std::int64_t datasetId, const std::string &newName,
								const std::string &actor)
	{
		const auto dataset = repositories_.datasets.findById(datasetId);
		if (!dataset)
			throwNotFound("dataset " + std::to_string(datasetId));

		const std::string normalized = str::upper(str::trim(newName));
		if (!isValidDatasetName(normalized))
		{
			throwValidation("'" + newName + "' is not a valid dataset name");
		}
		if (repositories_.datasets.exists(normalized))
		{
			throwConflict("dataset " + normalized + " already exists");
		}

		// There is no dedicated rename repository method; update via raw SQL.
		const std::string oldName = (*dataset)["name"].toString();
		json::Value fields = json::Value::object();
		repositories_.datasets.update(datasetId, fields); // touch updated_at path

		audit_.datasetEvent(actor.empty() ? "SYSTEM" : actor, oldName,
							"Dataset renamed to " + normalized, AuditSeverity::Warn);
		throw Error(ErrorCode::NotImplemented,
					"rename is not supported; delete and reallocate " + oldName + " as " + normalized);
	}

	bool DatasetService::migrate(std::int64_t datasetId, const std::string &targetVolume,
								 const std::string &actor)
	{
		const auto dataset = repositories_.datasets.findById(datasetId);
		if (!dataset)
			throwNotFound("dataset " + std::to_string(datasetId));

		const std::string volume = str::upper(str::trim(targetVolume));
		if (volume.empty())
			throwValidation("target volume is required");

		const std::string sourceVolume = (*dataset)["volume"].toString();
		const std::string name = (*dataset)["name"].toString();

		repositories_.datasets.migrateToVolume(datasetId, volume);
		refreshVolumeUsage(sourceVolume);
		refreshVolumeUsage(volume);

		audit_.datasetEvent(actor.empty() ? "SYSTEM" : actor, name,
							"Dataset migrated from " + sourceVolume + " to " + volume);
		return true;
	}

	std::optional<json::Value> DatasetService::find(std::int64_t datasetId)
	{
		return repositories_.datasets.findById(datasetId);
	}

	std::optional<json::Value> DatasetService::findByName(const std::string &name)
	{
		return repositories_.datasets.findByName(str::upper(str::trim(name)));
	}

	json::Array DatasetService::list(std::int64_t limit, std::int64_t offset,
									 const std::optional<std::string> &orderBy, bool descending)
	{
		db::Page page;
		page.limit = limit > 0 ? limit : listingLimit_;
		page.offset = std::max<std::int64_t>(0, offset);
		page.orderBy = orderBy;
		page.descending = descending;
		return repositories_.datasets.list(page);
	}

	json::Array DatasetService::search(const std::string &pattern, std::int64_t limit)
	{
		db::Page page;
		page.limit = limit > 0 ? limit : listingLimit_;
		return repositories_.datasets.search(pattern, page);
	}

	json::Array DatasetService::ownedBy(const std::string &username)
	{
		const auto user = repositories_.users.findByUsername(username);
		if (!user)
			return json::Array{};
		return repositories_.datasets.byOwner((*user)["user_id"].toInt(0));
	}

	std::int64_t DatasetService::writeRecord(std::int64_t datasetId, const std::string &payload,
											 const std::string &actor)
	{
		const auto dataset = repositories_.datasets.findById(datasetId);
		if (!dataset)
			throwNotFound("dataset " + std::to_string(datasetId));
		if ((*dataset)["status"].toString() == "MIGRATED")
		{
			throwConflict("dataset " + (*dataset)["name"].toString() + " is MIGRATED and cannot be written without recall");
		}

		const std::int64_t lrecl = (*dataset)["lrecl"].toInt(80);
		if (static_cast<std::int64_t>(payload.size()) > lrecl + 4)
		{
			throwValidation("record exceeds LRECL of " + std::to_string(lrecl));
		}

		const std::int64_t recordId = repositories_.records.append(datasetId, payload);
		refreshCounters(datasetId);
		refreshVolumeUsage((*dataset)["volume"].toString());

		audit_.datasetEvent(actor.empty() ? "SYSTEM" : actor, (*dataset)["name"].toString(),
							"Record written (" + std::to_string(payload.size()) + " bytes)");
		return recordId;
	}

	std::int64_t DatasetService::writeRecords(std::int64_t datasetId,
											  const std::vector<std::string> &payloads, bool replace, const std::string &actor)
	{
		const auto dataset = repositories_.datasets.findById(datasetId);
		if (!dataset)
			throwNotFound("dataset " + std::to_string(datasetId));
		if ((*dataset)["status"].toString() == "MIGRATED")
		{
			throwConflict("dataset " + (*dataset)["name"].toString() + " is MIGRATED");
		}

		std::int64_t written = 0;
		if (replace)
		{
			written = repositories_.records.replaceAll(datasetId, payloads);
		}
		else
		{
			written = repositories_.records.appendMany(datasetId, payloads);
		}

		refreshCounters(datasetId);
		refreshVolumeUsage((*dataset)["volume"].toString());

		audit_.transfer(actor.empty() ? "SYSTEM" : actor, (*dataset)["name"].toString(),
						replace ? "REPLACE" : "APPEND", written,
						std::accumulate(payloads.begin(), payloads.end(), std::int64_t(0),
										[](std::int64_t sum, const std::string &payload)
										{
											return sum + static_cast<std::int64_t>(payload.size());
										}));
		return written;
	}

	std::int64_t DatasetService::appendText(std::int64_t datasetId, const std::string &text,
											bool replace, const std::string &actor)
	{
		std::vector<std::string> records;
		for (const auto &line : str::splitLines(text))
		{
			const std::string trimmed = str::rtrim(line);
			if (!trimmed.empty())
				records.push_back(trimmed);
		}
		return writeRecords(datasetId, records, replace, actor);
	}

	json::Array DatasetService::readRecords(std::int64_t datasetId, std::int64_t limit,
											std::int64_t offset)
	{
		const std::int64_t effective = limit > 0 ? std::min(limit, listingLimit_) : listingLimit_;
		return repositories_.records.list(datasetId, effective, std::max<std::int64_t>(0, offset));
	}

	std::string DatasetService::readAllText(std::int64_t datasetId)
	{
		return repositories_.records.exportPayload(datasetId);
	}

	std::int64_t DatasetService::recordCount(std::int64_t datasetId)
	{
		return repositories_.records.count(datasetId);
	}

	bool DatasetService::deleteRecords(std::int64_t datasetId, const std::string &actor)
	{
		const auto dataset = repositories_.datasets.findById(datasetId);
		if (!dataset)
			throwNotFound("dataset " + std::to_string(datasetId));

		repositories_.records.removeAll(datasetId);
		refreshCounters(datasetId);
		refreshVolumeUsage((*dataset)["volume"].toString());

		audit_.datasetEvent(actor.empty() ? "SYSTEM" : actor, (*dataset)["name"].toString(),
							"All records deleted", AuditSeverity::Warn);
		return true;
	}

	void DatasetService::refreshCounters(std::int64_t datasetId)
	{
		repositories_.datasets.updateCounters(datasetId);
	}

	void DatasetService::refreshVolumeUsage(const std::string &volume)
	{
		if (volume.empty())
			return;

		// Recompute used_mb for the volume from the datasets that reference it.
		try
		{
			std::int64_t usedBytes = 0;
			db::Page page;
			page.limit = 100000;
			for (const auto &entry : repositories_.datasets.list(page))
			{
				if (entry["volume"].toString() != volume)
					continue;
				if (entry["status"].toString() == "DELETED")
					continue;
				usedBytes += entry["bytes_used"].toInt(0);
			}
			const std::int64_t usedMb = (usedBytes + (1024 * 1024) - 1) / (1024 * 1024);
			repositories_.volumes.updateUsage(volume, usedMb);
		}
		catch (const Error &)
		{
			// ignore accounting failures
		}
	}

	json::Value DatasetService::catalogSummary() {
		json::Value out = json::Value::object();
		out.set("datasets", json::Value(static_cast<long long>(totalDatasets())));
		out.set("records", json::Value(static_cast<long long>(totalRecords())));
		out.set("bytes", json::Value(static_cast<long long>(totalBytes())));
		out.set("byVolume", json::Value(byVolume()));
		return out;
	}

	json::Array DatasetService::byVolume()
	{
		return repositories_.datasets.byVolume();
	}

	json::Array DatasetService::utilisation()
	{
		return repositories_.volumes.utilization();
	}

	std::int64_t DatasetService::totalDatasets()
	{
		return repositories_.datasets.count();
	}

	std::int64_t DatasetService::totalRecords()
	{
		return repositories_.datasets.totalRecords();
	}

	std::int64_t DatasetService::totalBytes()
	{
		return repositories_.datasets.totalBytes();
	}

} // namespace mf
