// ---------------------------------------------------------------------------
// Batch job scheduler (JES2-flavoured).
//
// Jobs are submitted to the queue, dispatched to a worker thread pool bounded
// by system.maxConcurrentJobs, and executed. Each "program" is a small
// simulated step that produces JCL-style output and a return code - IEFBR14,
// IEBCOPY, ACCTPOST, RPTPRINT, LOADDATA, IDCAMS, SORT.
// ---------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mf/core/audit.hpp"
#include "mf/core/logger.hpp"
#include "mf/db/repository.hpp"
#include "mf/util/json.hpp"

namespace mf
{

	// Known job programs and their simulated characteristics.
	struct ProgramProfile
	{
		std::string name;
		std::string description;
		std::int64_t baseCpuMs = 100;
		int typicalReturnCode = 0;
	};

	const std::map<std::string, ProgramProfile> &programCatalog();
	ProgramProfile programProfile(const std::string &name);

	struct JobSubmission
	{
		std::string jobName;
		std::string jobClass = "A";
		std::string stepName = "STEP0001";
		std::string program = "IEFBR14";
		std::int64_t priority = 5;
		std::int64_t submittedBy = 0;
		std::vector<std::string> parameters; // e.g. dataset names the step touches
	};

	struct JobOutcome
	{
		std::int64_t jobId = 0;
		std::string jobName;
		std::string status; // COMPLETE | ABEND | CANCELLED
		int returnCode = 0;
		std::int64_t cpuMs = 0;
		std::string output;
	};

	struct SchedulerStats
	{
		std::int64_t submitted = 0;
		std::int64_t completed = 0;
		std::int64_t abended = 0;
		std::int64_t cancelled = 0;
		std::int64_t running = 0;
		std::int64_t queued = 0;
		int workers = 0;
		bool accepting = true;
	};

	class JobScheduler
	{
	public:
		JobScheduler(db::Repositories &repositories, AuditService &audit, Logger &logger);
		~JobScheduler();

		// Start / stop the worker pool.
		void start(int workers);
		void stop();

		bool running() const { return running_.load(); }
		int workerCount() const { return static_cast<int>(workers_.size()); }

		void setMaxConcurrent(int workers);
		void setAccepting(bool accepting) { accepting_.store(accepting); }

		// Submit a job; returns the new job id. Records it in the jobs table.
		std::int64_t submit(const JobSubmission &submission, const std::string &actor = "");

		// Cancel a queued or running job.
		bool cancel(std::int64_t jobId, const std::string &actor = "");

		// Wait for a specific job to reach a terminal state (used by tests and
		// by the synchronous submit path).
		std::optional<JobOutcome> waitFor(std::int64_t jobId, int timeoutMs = 30000);

		// Run a job body inline (no queue) - used by the CLI and unit tests.
		JobOutcome execute(const JobSubmission &submission);

		SchedulerStats stats();
		json::Value queueSnapshot();
		json::Value summary();

		void purgeCompleted(int olderThanDays);

		// Hook invoked whenever a job reaches a terminal state.
		void setCompletionHook(std::function<void(const JobOutcome &)> hook);

	private:
		struct QueueEntry
		{
			std::int64_t jobId;
			JobSubmission submission;
			std::string actor;
		};

		void workerLoop(int workerIndex);
		JobOutcome runJob(std::int64_t jobId, const JobSubmission &submission, const std::string &actor);
		std::string buildOutput(const JobSubmission &submission, int returnCode, std::int64_t cpuMs);
		int evaluateReturnCode(const JobSubmission &submission, std::string &diagnostic);

		db::Repositories &repositories_;
		AuditService &audit_;
		Logger &logger_;

		std::mutex mutex_;
		std::condition_variable condition_;
		std::deque<QueueEntry> queue_;
		std::vector<std::thread> workers_;
		std::atomic<bool> running_{false};
		std::atomic<bool> accepting_{true};
		std::atomic<int> maxWorkers_{4};
		int activeWorkers_ = 0;

		std::mutex outcomeMutex_;
		std::condition_variable outcomeCondition_;
		std::map<std::int64_t, JobOutcome> outcomes_;

		std::atomic<std::int64_t> submitted_{0};
		std::atomic<std::int64_t> completed_{0};
		std::atomic<std::int64_t> abended_{0};
		std::atomic<std::int64_t> cancelled_{0};

		std::mutex hookMutex_;
		std::function<void(const JobOutcome &)> completionHook_;
	};

} // namespace mf
