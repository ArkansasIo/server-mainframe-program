-- ---------------------------------------------------------------------------
-- Mainframe Server System - operational and capacity reporting views
--
-- sql/views.sql carries the five original summary views. This file adds the
-- views the operator dashboard and the capacity planner need: health, alerts,
-- queue depth, storage pressure and throughput.
--
-- Each view is dropped and recreated so edits take effect on the next run of
-- the migrator (views are revision 003; re-running applies the newest text).
-- ---------------------------------------------------------------------------

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- System health - one row per system image, with its asset counts.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_system_health;
CREATE VIEW v_system_health AS
SELECT
    s.system_id                                     AS system_id,
    s.name                                          AS name,
    s.sysplex                                       AS sysplex,
    s.region                                        AS region,
    s.status                                        AS status,
    s.version                                       AS version,
    (SELECT COUNT(*) FROM users WHERE status = 'ACTIVE')            AS active_users,
    (SELECT COUNT(*) FROM datasets WHERE status = 'AVAILABLE')      AS available_datasets,
    (SELECT COUNT(*) FROM jobs WHERE status IN ('QUEUED','RUNNING')) AS open_jobs,
    (SELECT COUNT(*) FROM volumes WHERE status = 'ONLINE')          AS online_volumes,
    s.updated_at                                    AS updated_at
FROM systems s
ORDER BY s.name;

-- ---------------------------------------------------------------------------
-- Open alerts - the unacknowledged, serious events only.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_open_alerts;
CREATE VIEW v_open_alerts AS
SELECT
    a.log_id                                        AS log_id,
    a.created_at                                    AS created_at,
    a.severity                                      AS severity,
    a.event_type                                    AS event_type,
    a.actor                                         AS actor,
    a.resource                                      AS resource,
    a.message                                       AS message
FROM audit_log a
WHERE a.severity IN ('ERROR', 'CRITICAL', 'WARN')
ORDER BY
    CASE a.severity
        WHEN 'CRITICAL' THEN 0
        WHEN 'ERROR'    THEN 1
        WHEN 'WARN'     THEN 2
        ELSE 3
    END,
    a.log_id DESC;

-- ---------------------------------------------------------------------------
-- Job queue depth by class and status - the JES2 "what is waiting" panel.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_job_queue_by_class;
CREATE VIEW v_job_queue_by_class AS
SELECT
    j.job_class                                     AS job_class,
    j.status                                        AS status,
    COUNT(*)                                        AS job_count,
    MIN(j.submitted_at)                             AS oldest_submitted,
    MAX(j.priority)                                 AS highest_priority,
    COALESCE(SUM(j.cpu_ms), 0)                      AS total_cpu_ms
FROM jobs j
GROUP BY j.job_class, j.status
ORDER BY j.job_class, j.status;

-- ---------------------------------------------------------------------------
-- Storage pressure - volumes closest to full, with their headroom.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_storage_pressure;
CREATE VIEW v_storage_pressure AS
SELECT
    v.serial                                        AS serial,
    v.device_type                                   AS device_type,
    v.status                                        AS status,
    v.capacity_mb                                   AS capacity_mb,
    v.used_mb                                       AS used_mb,
    (v.capacity_mb - v.used_mb)                     AS free_mb,
    ROUND(100.0 * v.used_mb / NULLIF(v.capacity_mb, 0), 2) AS pct_used,
    COUNT(d.dataset_id)                             AS dataset_count,
    COALESCE(SUM(d.record_count), 0)                AS records,
    CASE
        WHEN v.used_mb >= v.capacity_mb            THEN 'FULL'
        WHEN v.used_mb >  v.capacity_mb * 9 / 10   THEN 'CRITICAL'
        WHEN v.used_mb >  v.capacity_mb * 3 / 4    THEN 'HIGH'
        WHEN v.used_mb >  v.capacity_mb / 2        THEN 'MODERATE'
        ELSE 'HEALTHY'
    END                                             AS pressure
FROM volumes v
LEFT JOIN datasets d ON d.volume = v.serial AND d.status <> 'DELETED'
GROUP BY v.volume_id
ORDER BY pct_used DESC;

-- ---------------------------------------------------------------------------
-- Throughput by hour - transaction volume, for the trend sparkline.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_throughput_hourly;
CREATE VIEW v_throughput_hourly AS
SELECT
    strftime('%Y-%m-%d %H', t.created_at)           AS hour,
    COUNT(*)                                        AS transactions,
    SUM(t.rows_read)                                AS rows_read,
    SUM(t.rows_written)                             AS rows_written,
    SUM(CASE WHEN t.status = 'OK' THEN 1 ELSE 0 END)     AS ok_count,
    SUM(CASE WHEN t.status <> 'OK' THEN 1 ELSE 0 END)    AS problem_count,
    ROUND(AVG(t.elapsed_ms), 1)                      AS avg_elapsed_ms,
    MAX(t.elapsed_ms)                                AS worst_elapsed_ms
FROM transactions t
GROUP BY hour
ORDER BY hour DESC;

-- ---------------------------------------------------------------------------
-- Terminal activity - which terminals are doing the work and how reliably.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_terminal_activity;
CREATE VIEW v_terminal_activity AS
SELECT
    t.terminal                                      AS terminal,
    COUNT(*)                                        AS transactions,
    SUM(CASE WHEN t.status = 'OK' THEN 1 ELSE 0 END)     AS ok_count,
    SUM(CASE WHEN t.status <> 'OK' THEN 1 ELSE 0 END)    AS problem_count,
    ROUND(100.0 * SUM(CASE WHEN t.status = 'OK' THEN 1 ELSE 0 END)
          / NULLIF(COUNT(*), 0), 2)                  AS success_pct,
    ROUND(AVG(t.elapsed_ms), 1)                      AS avg_elapsed_ms,
    MAX(t.created_at)                                AS last_seen
FROM transactions t
GROUP BY t.terminal
ORDER BY transactions DESC;

-- ---------------------------------------------------------------------------
-- Dataset catalogue with its owner resolved - the panel the operator browses.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_dataset_catalog;
CREATE VIEW v_dataset_catalog AS
SELECT
    d.dataset_id                                    AS dataset_id,
    d.name                                          AS name,
    d.dsorg                                         AS dsorg,
    d.recfm                                         AS recfm,
    d.lrecl                                         AS lrecl,
    d.blksize                                       AS blksize,
    d.volume                                        AS volume,
    d.record_count                                  AS record_count,
    d.bytes_used                                    AS bytes_used,
    d.status                                        AS status,
    COALESCE(u.username, '(unowned)')               AS owner,
    COALESCE(u.role, '-')                           AS owner_role,
    d.created_at                                    AS created_at,
    d.updated_at                                    AS updated_at
FROM datasets d
LEFT JOIN users u ON u.user_id = d.owner_user_id
ORDER BY d.name;

-- ---------------------------------------------------------------------------
-- Orphaned records - records whose dataset has gone. An integrity check.
-- A healthy catalog returns no rows.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_orphaned_records;
CREATE VIEW v_orphaned_records AS
SELECT
    r.record_id                                     AS record_id,
    r.dataset_id                                    AS dataset_id,
    r.sequence                                      AS sequence,
    LENGTH(r.payload)                               AS payload_bytes,
    r.created_at                                    AS created_at
FROM dataset_records r
LEFT JOIN datasets d ON d.dataset_id = r.dataset_id
WHERE d.dataset_id IS NULL;

-- ---------------------------------------------------------------------------
-- Counter drift - datasets whose stored counters disagree with their records.
-- This is the check the triggers in sql/triggers.sql keep honest; any row here
-- means a writer bypassed them. A healthy catalog returns no rows.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_counter_drift;
CREATE VIEW v_counter_drift AS
SELECT
    d.dataset_id                                    AS dataset_id,
    d.name                                          AS name,
    d.record_count                                  AS stored_records,
    COALESCE(actual.records, 0)                     AS actual_records,
    d.bytes_used                                    AS stored_bytes,
    COALESCE(actual.bytes, 0)                       AS actual_bytes
FROM datasets d
LEFT JOIN (
    SELECT
        dataset_id,
        COUNT(*)                    AS records,
        SUM(LENGTH(payload))        AS bytes
    FROM dataset_records
    GROUP BY dataset_id
) actual ON actual.dataset_id = d.dataset_id
WHERE d.record_count <> COALESCE(actual.records, 0)
   OR d.bytes_used   <> COALESCE(actual.bytes, 0);

-- ---------------------------------------------------------------------------
-- Operator scoreboard - actions per operator, for the shift report.
-- ---------------------------------------------------------------------------
DROP VIEW IF EXISTS v_operator_scoreboard;
CREATE VIEW v_operator_scoreboard AS
SELECT
    u.username                                      AS username,
    u.role                                          AS role,
    u.status                                        AS status,
    u.last_logon                                    AS last_logon,
    (SELECT COUNT(*) FROM audit_log a WHERE a.actor = u.username)       AS audit_events,
    (SELECT COUNT(*) FROM jobs j WHERE j.submitted_by = u.user_id)      AS jobs_submitted,
    (SELECT COUNT(*) FROM transactions t WHERE t.user_id = u.user_id)   AS transactions,
    (SELECT COUNT(*) FROM datasets d WHERE d.owner_user_id = u.user_id) AS datasets_owned
FROM users u
ORDER BY audit_events DESC, u.username;
