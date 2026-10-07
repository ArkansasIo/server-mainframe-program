-- ---------------------------------------------------------------------------
-- Mainframe Server System - capacity and performance reporting queries
--
-- Companion to sql/queries/reporting.sql, which holds the operational
-- day-to-day reports. This file answers "is the system going to run out of
-- something soon, and what is costing us".
--
-- Run one at a time, e.g.:
--   mf-db query --file sql/queries/capacity.sql
-- ---------------------------------------------------------------------------

-- name: volume_growth_projection
-- Days of headroom left per volume, from the last 30 days of writes.
-- Assumes the average daily growth of the dataset bytes on that volume.
SELECT
    v.serial                                            AS volume,
    v.capacity_mb                                       AS capacity_mb,
    v.used_mb                                           AS used_mb,
    (v.capacity_mb - v.used_mb)                         AS free_mb,
    COALESCE(ROUND(daily.growth_mb, 3), 0)              AS growth_mb_per_day,
    CASE
        WHEN COALESCE(daily.growth_mb, 0) <= 0          THEN NULL
        ELSE CAST((v.capacity_mb - v.used_mb)
                  / daily.growth_mb AS INTEGER)
    END                                                 AS days_remaining
FROM volumes v
LEFT JOIN (
    SELECT
        d.volume,
        SUM(LENGTH(r.payload)) / 30.0 / 1048576.0       AS growth_mb
    FROM dataset_records r
    JOIN datasets d ON d.dataset_id = r.dataset_id
    WHERE r.created_at >= datetime('now', '-30 days')
    GROUP BY d.volume
) daily ON daily.volume = v.serial
ORDER BY days_remaining IS NULL, days_remaining ASC;

-- name: largest_datasets
-- The datasets worth archiving first.
SELECT
    d.name                                              AS name,
    d.dsorg                                             AS dsorg,
    d.recfm                                             AS recfm,
    d.volume                                            AS volume,
    d.record_count                                      AS records,
    d.bytes_used                                        AS bytes_used,
    ROUND(d.bytes_used / 1048576.0, 2)                  AS size_mb,
    COALESCE(u.username, '(unowned)')                   AS owner,
    d.updated_at                                        AS last_updated
FROM datasets d
LEFT JOIN users u ON u.user_id = d.owner_user_id
WHERE d.status <> 'DELETED'
ORDER BY d.bytes_used DESC
LIMIT 25;

-- name: datasets_not_touched
-- Candidate archive/delete material: available but untouched for 180 days.
SELECT
    d.name                                              AS name,
    d.volume                                            AS volume,
    d.record_count                                      AS records,
    d.bytes_used                                        AS bytes_used,
    d.status                                            AS status,
    d.updated_at                                        AS last_updated,
    CAST(julianday('now') - julianday(d.updated_at) AS INTEGER) AS days_idle
FROM datasets d
WHERE d.status = 'AVAILABLE'
  AND d.updated_at < datetime('now', '-180 days')
ORDER BY days_idle DESC;

-- name: job_runtime_profile
-- Per program: how long it takes and how often it fails, for tuning.
SELECT
    j.program                                           AS program,
    COUNT(*)                                            AS executions,
    SUM(CASE WHEN j.return_code = 0 THEN 1 ELSE 0 END)  AS success_count,
    SUM(CASE WHEN j.return_code <> 0 THEN 1 ELSE 0 END) AS failure_count,
    ROUND(100.0 * SUM(CASE WHEN j.return_code = 0 THEN 1 ELSE 0 END)
          / NULLIF(COUNT(*), 0), 2)                     AS success_pct,
    ROUND(AVG(j.cpu_ms), 0)                             AS avg_cpu_ms,
    MAX(j.cpu_ms)                                       AS worst_cpu_ms,
    SUM(j.cpu_ms)                                       AS total_cpu_ms
FROM jobs j
GROUP BY j.program
ORDER BY total_cpu_ms DESC;

-- name: slowest_transactions
-- The 20 worst transaction times, for finding the hot spots.
SELECT
    t.txn_id                                            AS txn_id,
    t.created_at                                        AS created_at,
    t.txn_code                                          AS txn_code,
    t.terminal                                          AS terminal,
    t.status                                            AS status,
    t.elapsed_ms                                        AS elapsed_ms,
    t.rows_read                                         AS rows_read,
    t.rows_written                                      AS rows_written,
    t.detail                                            AS detail
FROM transactions t
ORDER BY t.elapsed_ms DESC
LIMIT 20;

-- name: transaction_percentiles
-- p50 / p90 / p99 latency per transaction code.
-- SQLite has no percentile function, so the nearest-rank position is used.
SELECT
    x.txn_code                                          AS txn_code,
    x.executions                                        AS executions,
    x.p50_ms                                            AS p50_ms,
    x.p90_ms                                            AS p90_ms,
    x.p99_ms                                            AS p99_ms,
    x.max_ms                                            AS max_ms
FROM (
    SELECT
        t.txn_code                                      AS txn_code,
        COUNT(*)                                        AS executions,
        (SELECT elapsed_ms FROM transactions t2
          WHERE t2.txn_code = t.txn_code ORDER BY t2.elapsed_ms
          LIMIT 1 OFFSET (COUNT(*) * 50 / 100))         AS p50_ms,
        (SELECT elapsed_ms FROM transactions t2
          WHERE t2.txn_code = t.txn_code ORDER BY t2.elapsed_ms
          LIMIT 1 OFFSET (COUNT(*) * 90 / 100))         AS p90_ms,
        (SELECT elapsed_ms FROM transactions t2
          WHERE t2.txn_code = t.txn_code ORDER BY t2.elapsed_ms
          LIMIT 1 OFFSET (COUNT(*) * 99 / 100))         AS p99_ms,
        MAX(t.elapsed_ms)                               AS max_ms
    FROM transactions t
    GROUP BY t.txn_code
) x
ORDER BY x.executions DESC;

-- name: system_parameter_report
-- Every scheduler and security parameter, grouped by its prefix.
SELECT
    SUBSTR(p.param_key, 1, INSTR(p.param_key, '.') - 1) AS group_name,
    p.param_key                                         AS param_key,
    p.param_value                                       AS param_value,
    p.description                                       AS description,
    p.updated_at                                        AS updated_at
FROM system_parameters p
WHERE INSTR(p.param_key, '.') > 0
ORDER BY group_name, p.param_key;

-- name: audit_volume_by_actor
-- Who is generating the log traffic - useful before setting retention.
SELECT
    a.actor                                             AS actor,
    COUNT(*)                                            AS events,
    SUM(CASE WHEN a.severity = 'CRITICAL' THEN 1 ELSE 0 END) AS critical,
    SUM(CASE WHEN a.severity = 'ERROR'    THEN 1 ELSE 0 END) AS errors,
    SUM(CASE WHEN a.severity = 'WARN'     THEN 1 ELSE 0 END) AS warnings,
    MIN(a.created_at)                                   AS first_seen,
    MAX(a.created_at)                                   AS last_seen
FROM audit_log a
GROUP BY a.actor
ORDER BY events DESC;

-- name: event_type_breakdown
-- What the system is actually logging, by type and severity.
SELECT
    a.event_type                                        AS event_type,
    a.severity                                          AS severity,
    COUNT(*)                                            AS events,
    ROUND(100.0 * COUNT(*) / (SELECT COUNT(*) FROM audit_log), 2) AS pct_of_log
FROM audit_log a
GROUP BY a.event_type, a.severity
ORDER BY events DESC;

-- name: integrity_dashboard
-- One row: the headline counts an operator wants on the wall.
SELECT
    (SELECT COUNT(*) FROM systems)                       AS systems,
    (SELECT COUNT(*) FROM users WHERE status = 'ACTIVE') AS active_users,
    (SELECT COUNT(*) FROM datasets WHERE status = 'AVAILABLE') AS available_datasets,
    (SELECT COUNT(*) FROM dataset_records)               AS total_records,
    (SELECT COUNT(*) FROM jobs WHERE status IN ('QUEUED','RUNNING')) AS open_jobs,
    (SELECT COUNT(*) FROM jobs WHERE status = 'ABEND')   AS abended_jobs,
    (SELECT COUNT(*) FROM transactions)                  AS transactions,
    (SELECT COUNT(*) FROM audit_log)                     AS audit_events,
    (SELECT COUNT(*) FROM v_open_alerts)                 AS open_alerts,
    (SELECT COUNT(*) FROM v_orphaned_records)            AS orphaned_records,
    (SELECT COUNT(*) FROM v_counter_drift)               AS counter_drift_rows;
