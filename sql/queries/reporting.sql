-- ---------------------------------------------------------------------------
-- Ad-hoc reporting queries
-- Run one at a time, e.g.:
--   node scripts/db-query.js --file sql/queries/reporting.sql
-- ---------------------------------------------------------------------------

-- name: job_queue_depth
-- Jobs currently waiting or running, most important first.
SELECT job_id, job_name, job_class, status, priority, submitted_at
FROM jobs
WHERE status IN ('QUEUED', 'RUNNING', 'HOLD')
ORDER BY priority DESC, submitted_at ASC;

-- name: dataset_top10_by_size
SELECT name, dsorg, recfm, record_count, bytes_used, volume
FROM datasets
WHERE status = 'AVAILABLE'
ORDER BY bytes_used DESC
LIMIT 10;

-- name: transaction_failures
SELECT txn_id, created_at, txn_code, terminal, status, detail
FROM transactions
WHERE status IN ('FAIL', 'ABEND')
ORDER BY created_at DESC;

-- name: volume_headroom
SELECT serial, device_type, capacity_mb, used_mb,
       (capacity_mb - used_mb) AS free_mb,
       ROUND(100.0 * used_mb / capacity_mb, 2) AS pct_used
FROM volumes
ORDER BY pct_used DESC;

-- name: users_without_recent_logon
SELECT username, full_name, role, status, last_logon
FROM users
WHERE last_logon IS NULL OR last_logon < datetime('now', '-90 days');

-- name: daily_audit_summary
SELECT date(created_at) AS day, severity, COUNT(*) AS events
FROM audit_log
GROUP BY day, severity
ORDER BY day DESC, severity;

-- name: dataset_catalog_search
-- Replace the placeholder before running.
SELECT dataset_id, name, dsorg, recfm, lrecl, owner_user_id
FROM datasets
WHERE name LIKE '%CUSTOMER%'
ORDER BY name;
