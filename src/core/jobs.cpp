#include "mf/core/jobs.hpp"

#include "mf/core/errors.hpp"
#include "mf/util/ids.hpp"
#include "mf/util/strings.hpp"
#include "mf/util/table.hpp"
#include "mf/util/time.hpp"

#include <algorithm>
#include <sstream>

namespace mf
{

	// -----------------------------------------------------------------------
	// Program catalog
	// -----------------------------------------------------------------------
	const std::map<std::string, ProgramProfile> &programCatalog()
	{
		static const std::map<std::string, ProgramProfile> catalog = {
			{"IEFBR14", {"IEFBR14", "Null program - allocates and returns", 25, 0}},
			{"IEBCOPY", {"IEBCOPY", "Dataset copy utility", 2400, 0}},
			{"IEBGENER", {"IEBGENER", "Sequential dataset generator", 1800, 0}},
			{"IDCAMS", {"IDCAMS", "Access method services (define / delete)", 900, 0}},
			{"SORT", {"SORT", "Record sort utility", 3200, 0}},
			{"ACCTPOST", {"ACCTPOST", "Post account ledger entries", 4100, 0}},
			{"RPTPRINT", {"RPTPRINT", "Monthly report generator", 5600, 4}},
			{"LOADDATA", {"LOADDATA", "Bulk dataset loader", 2200, 8}},
			{"SYSPROG", {"SYSPROG", "System programming step", 1500, 0}},
			{"SMFDUMP", {"SMFDUMP", "SMF record offload", 2600, 0}},
		};
		return catalog;
	}

	ProgramProfile programProfile(const std::string &name)
	{
		const std::string upper = str::upper(str::trim(name));
		const auto &catalog = programCatalog();
		const auto it = catalog.find(upper);
		if (it != catalog.end())
			return it->second;
		return ProgramProfile{upper.empty() ? "IEFBR14" : upper, "User supplied step", 500, 0};
	}

	// -----------------------------------------------------------------------
	// JobScheduler
	// -----------------------------------------------------------------------
	JobScheduler::JobScheduler(db::Repositories &repositories, AuditService &audit, Logger &logger)
		: repositories_(repositories), audit_(audit), logger_(logger)
	{
	}

	JobScheduler::~JobScheduler()
	{
		stop();
	}

	void JobScheduler::start(int workers)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (running_.load())
			return;

		const int count = workers > 0 ? workers : maxWorkers_.load();
		maxWorkers_.store(count);
		running_.store(true);
		workers_.clear();

		for (int i = 0; i < count; ++i)
		{
			workers_.emplace_back([this, i]()
								  { workerLoop(i); });
		}
		logger_.info("job scheduler started", json::Value(static_cast<long long>(count)));
	}

	void JobScheduler::stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (!running_.load())
				return;
			running_.store(false);
		}
		condition_.notify_all();
		for (auto &worker : workers_)
		{
			if (worker.joinable())
				worker.join();
		}
		workers_.clear();
		logger_.info("job scheduler stopped");
	}

	void JobScheduler::setMaxConcurrent(int workers)
	{
		if (workers < 1)
			workers = 1;
		maxWorkers_.store(workers);

		// Grow the pool immediately; shrinking takes effect as workers idle out.
		std::lock_guard<std::mutex> lock(mutex_);
		if (!running_.load())
			return;
		while (static_cast<int>(workers_.size()) < workers)
		{
			const int index = static_cast<int>(workers_.size());
			workers_.emplace_back([this, index]()
								  { workerLoop(index); });
		}
		condition_.notify_all();
	}

	std::int64_t JobScheduler::submit(const JobSubmission &submission, const std::string &actor)
	{
		if (!accepting_.load())
		{
			throw Error(ErrorCode::Unavailable, "job queue is not accepting submissions");
		}

		JobSubmission normalized = submission;
		normalized.jobName = str::upper(str::trim(normalized.jobName));
		normalized.jobClass = str::upper(str::trim(normalized.jobClass));
		normalized.program = str::upper(str::trim(normalized.program));

		if (normalized.jobName.empty())
			throwValidation("job name is required");
		if (normalized.jobName.size() > 8)
			throwValidation("job name must be at most 8 characters");
		if (normalized.jobClass.empty())
			normalized.jobClass = "A";
		if (normalized.program.empty())
			normalized.program = "IEFBR14";
		if (normalized.priority < 1)
			normalized.priority = 1;
		if (normalized.priority > 15)
			normalized.priority = 15;

		json::Value payload = json::Value::object();
		payload.set("job_name", json::Value(normalized.jobName));
		payload.set("job_class", json::Value(normalized.jobClass));
		payload.set("step_name", json::Value(normalized.stepName));
		payload.set("program", json::Value(normalized.program));
		payload.set("priority", json::Value(static_cast<long long>(normalized.priority)));
		payload.set("submitted_by", normalized.submittedBy > 0
										? json::Value(static_cast<long long>(normalized.submittedBy))
										: json::Value());

		const std::int64_t jobId = repositories_.jobs.create(payload);
		if (jobId <= 0)
			throwDatabase("failed to queue job " + normalized.jobName);

		submitted_.fetch_add(1);
		audit_.jobEvent(actor.empty() ? "SYSTEM" : actor, normalized.jobName,
						"Job submitted to class " + normalized.jobClass + " (program " + normalized.program + ", priority " + std::to_string(normalized.priority) + ")");

		if (running_.load())
		{
			{
				std::lock_guard<std::mutex> lock(mutex_);
				queue_.push_back(QueueEntry{jobId, normalized, actor});
			}
			condition_.notify_one();
		}
		else
		{
			// Scheduler not started - run inline so the caller still gets output.
			runJob(jobId, normalized, actor);
		}

		return jobId;
	}

	bool JobScheduler::cancel(std::int64_t jobId, const std::string &actor)
	{
		const auto job = repositories_.jobs.findById(jobId);
		if (!job)
			throwNotFound("job " + std::to_string(jobId));

		const std::string status = (*job)["status"].toString();
		if (status == "COMPLETE" || status == "ABEND" || status == "CANCELLED")
		{
			throwConflict("job " + std::to_string(jobId) + " already ended with status " + status);
		}

		// Remove it from the queue if it has not started yet.
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
										[jobId](const QueueEntry &entry)
										{ return entry.jobId == jobId; }),
						 queue_.end());
		}

		repositories_.jobs.cancel(jobId);
		cancelled_.fetch_add(1);

		JobOutcome outcome;
		outcome.jobId = jobId;
		outcome.jobName = (*job)["job_name"].toString();
		outcome.status = "CANCELLED";
		outcome.returnCode = 16;
		outcome.output = "JOB " + outcome.jobName + " CANCELLED BY OPERATOR";

		{
			std::lock_guard<std::mutex> lock(outcomeMutex_);
			outcomes_[jobId] = outcome;
		}
		outcomeCondition_.notify_all();

		audit_.jobEvent(actor.empty() ? "SYSTEM" : actor, outcome.jobName,
						"Job cancelled", AuditSeverity::Warn);
		return true;
	}

	std::optional<JobOutcome> JobScheduler::waitFor(std::int64_t jobId, int timeoutMs)
	{
		std::unique_lock<std::mutex> lock(outcomeMutex_);
		const bool ready = outcomeCondition_.wait_for(lock,
													  std::chrono::milliseconds(timeoutMs),
													  [this, jobId]()
													  { return outcomes_.count(jobId) > 0; });
		if (!ready)
			return std::nullopt;
		return outcomes_[jobId];
	}

	void JobScheduler::workerLoop(int workerIndex)
	{
		const Logger workerLog = logger_.child(json::Value::object());
		workerLog.debug("job worker online", json::Value(static_cast<long long>(workerIndex)));

		while (true)
		{
			QueueEntry entry;
			bool haveWork = false;

			// Drain the queue while this worker is under the concurrency cap.
			{
				std::unique_lock<std::mutex> lock(mutex_);
				condition_.wait(lock, [this, workerIndex]()
								{ return !running_.load() || (!queue_.empty() && workerIndex < maxWorkers_.load()); });

				if (!running_.load() && queue_.empty())
					return;

				if (!queue_.empty() && workerIndex < maxWorkers_.load())
				{
					// Highest priority first, then oldest.
					auto best = std::max_element(queue_.begin(), queue_.end(),
												 [](const QueueEntry &a, const QueueEntry &b)
												 {
													 if (a.submission.priority != b.submission.priority)
													 {
														 return a.submission.priority < b.submission.priority;
													 }
													 return a.jobId > b.jobId;
												 });
					entry = *best;
					queue_.erase(best);
					haveWork = true;
					++activeWorkers_;
				}
			}

			if (!haveWork)
			{
				if (!running_.load())
					return;
				continue;
			}

			runJob(entry.jobId, entry.submission, entry.actor);

			{
				std::lock_guard<std::mutex> lock(mutex_);
				--activeWorkers_;
			}
		}
	}

	JobOutcome JobScheduler::runJob(std::int64_t jobId, const JobSubmission &submission,
									const std::string &actor)
	{
		const std::string actorName = actor.empty() ? "SYSTEM" : actor;
		timeutil::Stopwatch stopwatch;

		repositories_.jobs.markRunning(jobId);

		std::string diagnostic;
		const int returnCode = evaluateReturnCode(submission, diagnostic);

		// Simulate CPU time proportional to the program profile.
		const ProgramProfile profile = programProfile(submission.program);
		const std::int64_t simulatedCpu = profile.baseCpuMs + static_cast<std::int64_t>(submission.parameters.size()) * 120;

		const std::string output = buildOutput(submission, returnCode, simulatedCpu);
		const std::int64_t elapsed = std::max<std::int64_t>(simulatedCpu, stopwatch.elapsedMs());

		if (returnCode == 0)
		{
			repositories_.jobs.markComplete(jobId, 0, elapsed, output);
			completed_.fetch_add(1);
		}
		else
		{
			repositories_.jobs.markAbend(jobId, returnCode, output);
			abended_.fetch_add(1);
		}

		JobOutcome outcome;
		outcome.jobId = jobId;
		outcome.jobName = submission.jobName;
		outcome.status = returnCode == 0 ? "COMPLETE" : "ABEND";
		outcome.returnCode = returnCode;
		outcome.cpuMs = elapsed;
		outcome.output = output;

		audit_.jobEvent(actorName, submission.jobName,
						"Job ended " + outcome.status + " RC=" + std::to_string(returnCode) + " CPU=" + timeutil::humanDuration(elapsed),
						returnCode == 0 ? AuditSeverity::Info : AuditSeverity::Error);

		{
			std::lock_guard<std::mutex> lock(outcomeMutex_);
			outcomes_[jobId] = outcome;
		}
		outcomeCondition_.notify_all();

		{
			std::lock_guard<std::mutex> lock(hookMutex_);
			if (completionHook_)
				completionHook_(outcome);
		}

		return outcome;
	}

	int JobScheduler::evaluateReturnCode(const JobSubmission &submission, std::string &diagnostic)
	{
		const ProgramProfile profile = programProfile(submission.program);

		// Programs that touch datasets fail when the dataset is missing - this
		// mirrors the seeded BADLOAD abend (IEC141I 013-18).
		static const std::vector<std::string> datasetPrograms = {
			"IEBCOPY", "IEBGENER", "ACCTPOST", "LOADDATA", "IDCAMS", "SORT", "RPTPRINT", "SMFDUMP"};

		const bool needsDatasets = std::find(datasetPrograms.begin(), datasetPrograms.end(),
											 profile.name) != datasetPrograms.end();

		if (needsDatasets && submission.parameters.empty())
		{
			diagnostic = "IEC141I 013-18 - DATASET NOT FOUND";
			return 8;
		}

		for (const auto &parameter : submission.parameters)
		{
			if (!repositories_.datasets.exists(str::upper(parameter)))
			{
				diagnostic = "IEC141I 013-18 - " + str::upper(parameter) + " NOT IN CATALOG";
				return 8;
			}
		}

		// LOADDATA legitimately abends on malformed input; make that deterministic
		// when no parameters were supplied (already handled above).
		if (profile.typicalReturnCode != 0 && submission.parameters.empty())
		{
			diagnostic = "STEP ENDED WITH CONDITION CODE " + std::to_string(profile.typicalReturnCode);
			return profile.typicalReturnCode;
		}

		diagnostic.clear();
		return 0;
	}

	std::string JobScheduler::buildOutput(const JobSubmission &submission, int returnCode,
										  std::int64_t cpuMs)
	{
		std::ostringstream out;
		const ProgramProfile profile = programProfile(submission.program);

		out << "JOB " << submission.jobName << " - CLASS " << submission.jobClass
			<< " - PROGRAM " << profile.name << "\n";
		out << "//" << submission.jobName << " JOB (" << submission.jobClass << "),'"
			<< submission.jobName << "',CLASS=" << submission.jobClass
			<< ",PRTY=" << submission.priority << "\n";
		out << "//" << submission.stepName << " EXEC PGM=" << profile.name << "\n";
		out << "//* " << profile.description << "\n";

		for (const auto &parameter : submission.parameters)
		{
			out << "//* DSN=" << str::upper(parameter) << "\n";
		}

		out << "IEF403I " << submission.jobName << " - STARTED\n";
		out << "IEF142I " << submission.jobName << " " << submission.stepName
			<< " - STEP WAS EXECUTED - COND CODE " << returnCode << "\n";

		if (returnCode == 0)
		{
			out << "JOB " << submission.jobName << " COMPLETED - CPU "
				<< timeutil::humanDuration(cpuMs) << "\n";
			out << "IEF404I " << submission.jobName << " - ENDED\n";
		}
		else
		{
			out << "IEC141I 013-18 - ABEND S0C7 OR DATASET ERROR\n";
			out << "ABEND CODE " << returnCode << " - JOB " << submission.jobName
				<< " TERMINATED\n";
		}

		return out.str();
	}

	JobOutcome JobScheduler::execute(const JobSubmission &submission)
	{
		// Synchronous, uncatalogued execution for the CLI and tests.
		JobSubmission normalized = submission;
		normalized.jobName = str::upper(str::trim(normalized.jobName));
		if (normalized.jobName.empty())
			normalized.jobName = "TMPJOB";

		JobOutcome outcome;
		outcome.jobName = normalized.jobName;

		std::string diagnostic;
		const int returnCode = evaluateReturnCode(normalized, diagnostic);
		const ProgramProfile profile = programProfile(normalized.program);
		const std::int64_t cpuMs = profile.baseCpuMs + static_cast<std::int64_t>(normalized.parameters.size()) * 120;

		outcome.returnCode = returnCode;
		outcome.status = returnCode == 0 ? "COMPLETE" : "ABEND";
		outcome.cpuMs = cpuMs;
		outcome.output = buildOutput(normalized, returnCode, cpuMs);
		return outcome;
	}

	SchedulerStats JobScheduler::stats()
	{
		SchedulerStats out;
		out.submitted = submitted_.load();
		out.completed = completed_.load();
		out.abended = abended_.load();
		out.cancelled = cancelled_.load();
		out.workers = workerCount();
		out.accepting = accepting_.load();

		{
			std::lock_guard<std::mutex> lock(mutex_);
			out.queued = static_cast<std::int64_t>(queue_.size());
			out.running = activeWorkers_;
		}
		return out;
	}

	json::Value JobScheduler::queueSnapshot()
	{
		json::Value out = json::Value::object();

		std::lock_guard<std::mutex> lock(mutex_);
		json::Array pending;
		for (const auto &entry : queue_)
		{
			json::Value item = json::Value::object();
			item.set("job_id", json::Value(static_cast<long long>(entry.jobId)));
			item.set("job_name", json::Value(entry.submission.jobName));
			item.set("job_class", json::Value(entry.submission.jobClass));
			item.set("program", json::Value(entry.submission.program));
			item.set("priority", json::Value(static_cast<long long>(entry.submission.priority)));
			pending.push_back(std::move(item));
		}
		out.set("pending", json::Value(std::move(pending)));
		return out;
	}

	json::Value JobScheduler::summary()
	{
		json::Value out = repositories_.jobs.summary();
		const SchedulerStats current = stats();
		json::Value scheduler = json::Value::object();
		scheduler.set("submitted", json::Value(static_cast<long long>(current.submitted)));
		scheduler.set("completed", json::Value(static_cast<long long>(current.completed)));
		scheduler.set("abended", json::Value(static_cast<long long>(current.abended)));
		scheduler.set("cancelled", json::Value(static_cast<long long>(current.cancelled)));
		scheduler.set("running", json::Value(static_cast<long long>(current.running)));
		scheduler.set("queued_in_memory", json::Value(static_cast<long long>(current.queued)));
		scheduler.set("workers", json::Value(static_cast<long long>(current.workers)));
		scheduler.set("accepting", json::Value(current.accepting));
		out.set("scheduler", scheduler);
		return out;
	}

	void JobScheduler::purgeCompleted(int olderThanDays)
	{
		repositories_.jobs.purge("COMPLETE", olderThanDays);
		repositories_.jobs.purge("CANCELLED", olderThanDays);
		logger_.info("purged completed jobs older than " + std::to_string(olderThanDays) + " days");
	}

	void JobScheduler::setCompletionHook(std::function<void(const JobOutcome &)> hook)
	{
		std::lock_guard<std::mutex> lock(hookMutex_);
		completionHook_ = std::move(hook);
	}

} // namespace mf
