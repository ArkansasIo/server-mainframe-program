-- ---------------------------------------------------------------------------
-- Mainframe Server System - reporting views
-- ---------------------------------------------------------------------------

DROP VIEW IF EXISTS v_job_summary;
CREATE VIEW v_job_summary AS
SELECT
    j.status                                   AS status,
    COUNT(*)                                   AS job_count,
    SUM(j.cpu_ms)                              AS total_cpu_ms,
    ROUND(AVG(j.cpu_ms), 1)                    AS avg_cpu_ms,
    SUM(CASE WHEN j.return_code <> 0 THEN 1 ELSE 0 END) AS failures
FROM jobs j
GROUP BY j.status;

DROP VIEW IF EXISTS v_dataset_usage;
CREATE VIEW v_dataset_usage AS
SELECT
    d.volume                                   AS volume,
    COUNT(*)                                   AS dataset_count,
    SUM(d.record_count)                        AS records,
    SUM(d.bytes_used)                          AS bytes_used,
    v.capacity_mb                              AS volume_capacity_mb,
    v.used_mb                                  AS volume_used_mb,
    ROUND(100.0 * v.used_mb / NULLIF(v.capacity_mb, 0), 2) AS volume_pct_used
FROM datasets d
LEFT JOIN volumes v ON v.serial = d.volume
WHERE d.status <> 'DELETED'
GROUP BY d.volume, v.capacity_mb, v.used_mb
ORDER BY bytes_used DESC;

DROP VIEW IF EXISTS v_transaction_activity;
CREATE VIEW v_transaction_activity AS
SELECT
    t.txn_code                                 AS txn_code,
    COUNT(*)                                   AS executions,
    SUM(t.rows_read)                           AS rows_read,
    SUM(t.rows_written)                        AS rows_written,
    ROUND(AVG(t.elapsed_ms), 1)                AS avg_elapsed_ms,
    SUM(CASE WHEN t.status = 'OK' THEN 1 ELSE 0 END) AS ok_count,
    SUM(CASE WHEN t.status <> 'OK' THEN 1 ELSE 0 END) AS problem_count
FROM transactions t
GROUP BY t.txn_code
ORDER BY executions DESC;

DROP VIEW IF EXISTS v_user_activity;
CREATE VIEW v_user_activity AS
SELECT
    u.username                                 AS username,
    u.role                                     AS role,
    u.status                                   AS status,
    u.last_logon                               AS last_logon,
    COUNT(DISTINCT d.dataset_id)               AS datasets_owned,
    COUNT(DISTINCT j.job_id)                   AS jobs_submitted
FROM users u
LEFT JOIN datasets d ON d.owner_user_id = u.user_id
LEFT JOIN jobs j     ON j.submitted_by  = u.user_id
GROUP BY u.user_id
ORDER BY u.username;

DROP VIEW IF EXISTS v_audit_recent;
CREATE VIEW v_audit_recent AS
SELECT
    log_id, created_at, severity, event_type, actor, resource, message
FROM audit_log
ORDER BY log_id DESC
LIMIT 500;
