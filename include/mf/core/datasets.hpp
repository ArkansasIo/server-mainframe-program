// ---------------------------------------------------------------------------
// Dataset (catalogued record collection) services: cataloguing, record I/O and
// volume accounting. Mirrors z/OS dataset semantics closely enough to be
// recognisable: PS / PDS / VSAM organisation, F / FB / VB record formats,
// LRECL / BLKSIZE, volumes and an AVAILABLE / MIGRATED / DELETED lifecycle.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mf/core/audit.hpp"
#include "mf/core/logger.hpp"
#include "mf/db/repository.hpp"
#include "mf/util/json.hpp"

namespace mf
{

	struct DatasetAttributes
	{
		std::string name;		  // MF1.PROD.CUSTOMER.MASTER
		std::string dsorg = "PS"; // PS | PO | PDS | VSAM
		std::string recfm = "FB"; // F | FB | VB | VBS | U
		std::int64_t lrecl = 80;
		std::int64_t blksize = 27920;
		std::string volume = "MFVOL1";
		std::int64_t ownerUserId = 0;
	};

	// Parsed dataset name -> qualifiers. "MF1.PROD.CUST" -> {MF1, PROD, CUST}
	std::vector<std::string> splitQualifiers(const std::string &name);
	std::string datasetPrefix(const std::string &name);
	bool isValidDatasetName(const std::string &name);
	bool isValidRecordFormat(const std::string &recfm);
	bool isValidOrganization(const std::string &dsorg);

	// Space accounting: records * (lrecl + 4 bytes of RDW/BDW overhead).
	std::int64_t estimateSpace(std::int64_t recordCount, std::int64_t lrecl);

	class DatasetService
	{
	public:
		DatasetService(db::Repositories &repositories, AuditService &audit, Logger &logger);

		// Cap the number of records returned for terminal listings.
		void setListingLimit(std::int64_t limit) { listingLimit_ = limit; }

		// -- catalogue ------------------------------------------------------
		json::Value create(const DatasetAttributes &attributes, const std::string &actor);
		bool remove(std::int64_t datasetId, bool purgeRecords, const std::string &actor);
		bool rename(std::int64_t datasetId, const std::string &newName, const std::string &actor);
		bool allocate(const std::string &name, const DatasetAttributes &attributes, const std::string &actor);
		bool migrate(std::int64_t datasetId, const std::string &targetVolume, const std::string &actor);

		std::optional<json::Value> find(std::int64_t datasetId);
		std::optional<json::Value> findByName(const std::string &name);
		json::Array list(std::int64_t limit, std::int64_t offset,
						 const std::optional<std::string> &orderBy, bool descending);
		json::Array search(const std::string &pattern, std::int64_t limit);
		json::Array ownedBy(const std::string &username);

		// -- records --------------------------------------------------------
		std::int64_t writeRecords(std::int64_t datasetId, const std::vector<std::string> &payloads,
								  bool replace, const std::string &actor);
		std::int64_t writeRecord(std::int64_t datasetId, const std::string &payload,
								 const std::string &actor);
		std::int64_t appendText(std::int64_t datasetId, const std::string &text,
								bool replace, const std::string &actor);

		json::Array readRecords(std::int64_t datasetId, std::int64_t limit, std::int64_t offset);
		std::string readAllText(std::int64_t datasetId);
		std::int64_t recordCount(std::int64_t datasetId);
		bool deleteRecords(std::int64_t datasetId, const std::string &actor);

		// -- reporting ------------------------------------------------------
		json::Value catalogSummary();
		json::Array byVolume();
		json::Array utilisation();

		// Totals across the catalog.
		std::int64_t totalDatasets();
		std::int64_t totalRecords();
		std::int64_t totalBytes();

	private:
		// Recompute record_count / bytes_used after a mutation.
		void refreshCounters(std::int64_t datasetId);
		// Recompute the owning volume's used_mb from the datasets that live on it.
		void refreshVolumeUsage(const std::string &volume);
		// Validate and normalise the incoming attributes.
		DatasetAttributes validate(const DatasetAttributes &attributes);

		db::Repositories &repositories_;
		AuditService &audit_;
		Logger &logger_;
		std::int64_t listingLimit_ = 500;
	};

} // namespace mf
