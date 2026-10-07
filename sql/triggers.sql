-- ---------------------------------------------------------------------------
-- Mainframe Server System - integrity and audit triggers
--
-- These enforce the invariants the application also enforces in C++, at the
-- database level. That belt-and-braces approach matters because the CLI tools
-- (scripts/db-*.js, mf-db) write to the same file and would otherwise be able
-- to leave the catalog inconsistent.
--
-- Rules modelled here:
--   1. updated_at is maintained automatically - no caller can forget it
--   2. dataset record_count and bytes_used stay in step with dataset_records
--   3. a volume's used_mb tracks the datasets that live on it
--   4. deleting a dataset cascades its records (already declared, restated)
--   5. every write to a dataset or a job is journalled into audit_log
--
-- DROP TRIGGER IF EXISTS before each CREATE keeps the file re-runnable.
-- ---------------------------------------------------------------------------

PRAGMA foreign_keys = ON;

-- ===========================================================================
-- 1. updated_at maintenance
-- ===========================================================================

DROP TRIGGER IF EXISTS trg_users_updated_at;
CREATE TRIGGER trg_users_updated_at
AFTER UPDATE OF username, full_name, department, role, status, password_hash ON users
FOR EACH ROW
BEGIN
    UPDATE users SET updated_at = datetime('now') WHERE user_id = OLD.user_id;
END;

DROP TRIGGER IF EXISTS trg_datasets_updated_at;
CREATE TRIGGER trg_datasets_updated_at
AFTER UPDATE OF name, dsorg, recfm, lrecl, blksize, volume, owner_user_id, status ON datasets
FOR EACH ROW
BEGIN
    UPDATE datasets SET updated_at = datetime('now') WHERE dataset_id = OLD.dataset_id;
END;

DROP TRIGGER IF EXISTS trg_systems_updated_at;
CREATE TRIGGER trg_systems_updated_at
AFTER UPDATE OF name, sysplex, region, status, version ON systems
FOR EACH ROW
BEGIN
    UPDATE systems SET updated_at = datetime('now') WHERE system_id = OLD.system_id;
END;

-- ===========================================================================
-- 2. dataset counters follow their records
--
-- bytes_used is the sum of the record lengths, which is what the volume
-- accounting and the capacity reports consume. Keeping it here means a record
-- inserted by any tool is reflected in the catalog immediately.
-- ===========================================================================

DROP TRIGGER IF EXISTS trg_records_after_insert;
CREATE TRIGGER trg_records_after_insert
AFTER INSERT ON dataset_records
FOR EACH ROW
BEGIN
    UPDATE datasets
       SET record_count = record_count + 1,
           bytes_used   = bytes_used + LENGTH(NEW.payload),
           updated_at   = datetime('now')
     WHERE dataset_id = NEW.dataset_id;
END;

DROP TRIGGER IF EXISTS trg_records_after_delete;
CREATE TRIGGER trg_records_after_delete
AFTER DELETE ON dataset_records
FOR EACH ROW
BEGIN
    UPDATE datasets
       SET record_count = MAX(0, record_count - 1),
           bytes_used   = MAX(0, bytes_used - LENGTH(OLD.payload)),
           updated_at   = datetime('now')
     WHERE dataset_id = OLD.dataset_id;
END;

DROP TRIGGER IF EXISTS trg_records_after_update;
CREATE TRIGGER trg_records_after_update
AFTER UPDATE OF payload ON dataset_records
FOR EACH ROW
BEGIN
    UPDATE datasets
       SET bytes_used = MAX(0, bytes_used - LENGTH(OLD.payload) + LENGTH(NEW.payload)),
           updated_at = datetime('now')
     WHERE dataset_id = NEW.dataset_id;
END;

-- ===========================================================================
-- 3. volume usage follows the datasets placed on it
--
-- used_mb is derived, so it is recomputed from the datasets rather than
-- incremented: that way a deletion or a migration cannot leave it drifting.
-- ===========================================================================

DROP TRIGGER IF EXISTS trg_datasets_volume_usage_insert;
CREATE TRIGGER trg_datasets_volume_usage_insert
AFTER INSERT ON datasets
FOR EACH ROW
WHEN NEW.status <> 'DELETED'
BEGIN
    UPDATE volumes
       SET used_mb = (
               SELECT COALESCE(SUM(bytes_used), 0) / 1048576
                 FROM datasets
                WHERE volume = NEW.volume AND status <> 'DELETED'
           )
     WHERE serial = NEW.volume;
END;

DROP TRIGGER IF EXISTS trg_datasets_volume_usage_update;
CREATE TRIGGER trg_datasets_volume_usage_update
AFTER UPDATE OF volume, status, bytes_used ON datasets
FOR EACH ROW
BEGIN
    -- Recompute both the old and the new volume: a dataset that moved between
    -- volumes must be subtracted from one and added to the other.
    UPDATE volumes
       SET used_mb = (
               SELECT COALESCE(SUM(bytes_used), 0) / 1048576
                 FROM datasets
                WHERE volume = OLD.volume AND status <> 'DELETED'
           )
     WHERE serial = OLD.volume;

    UPDATE volumes
       SET used_mb = (
               SELECT COALESCE(SUM(bytes_used), 0) / 1048576
                 FROM datasets
                WHERE volume = NEW.volume AND status <> 'DELETED'
           )
     WHERE serial = NEW.volume;

    -- A volume that has filled up is marked FULL so the allocator avoids it.
    UPDATE volumes
       SET status = CASE
               WHEN used_mb >= capacity_mb AND status = 'ONLINE' THEN 'FULL'
               WHEN used_mb <  capacity_mb AND status = 'FULL'   THEN 'ONLINE'
               ELSE status
           END
     WHERE serial IN (OLD.volume, NEW.volume);
END;

DROP TRIGGER IF EXISTS trg_datasets_volume_usage_delete;
CREATE TRIGGER trg_datasets_volume_usage_delete
AFTER DELETE ON datasets
FOR EACH ROW
BEGIN
    UPDATE volumes
       SET used_mb = (
               SELECT COALESCE(SUM(bytes_used), 0) / 1048576
                 FROM datasets
                WHERE volume = OLD.volume AND status <> 'DELETED'
           )
     WHERE serial = OLD.volume;
END;

-- ===========================================================================
-- 4. audit journal
--
-- Every interesting write is copied into audit_log so the trail is complete
-- even when the writer was a script that never called the C++ audit service.
-- ===========================================================================

DROP TRIGGER IF EXISTS trg_datasets_audit_insert;
CREATE TRIGGER trg_datasets_audit_insert
AFTER INSERT ON datasets
FOR EACH ROW
BEGIN
    INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata)
    VALUES ('DATASET', 'INFO', 'SYSTEM', NEW.name,
            'Dataset allocated on volume ' || NEW.volume,
            '{"action":"create","dsorg":"' || NEW.dsorg || '","recfm":"' || NEW.recfm || '"}');
END;

DROP TRIGGER IF EXISTS trg_datasets_audit_update;
CREATE TRIGGER trg_datasets_audit_update
AFTER UPDATE OF status ON datasets
FOR EACH ROW
WHEN OLD.status <> NEW.status
BEGIN
    INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata)
    VALUES ('DATASET',
            CASE WHEN NEW.status = 'DELETED' THEN 'WARN' ELSE 'INFO' END,
            'SYSTEM', NEW.name,
            'Dataset status ' || OLD.status || ' -> ' || NEW.status,
            '{"action":"status","from":"' || OLD.status || '","to":"' || NEW.status || '"}');
END;

DROP TRIGGER IF EXISTS trg_datasets_audit_delete;
CREATE TRIGGER trg_datasets_audit_delete
AFTER DELETE ON datasets
FOR EACH ROW
BEGIN
    INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata)
    VALUES ('DATASET', 'WARN', 'SYSTEM', OLD.name,
            'Dataset removed from the catalog',
            '{"action":"delete","volume":"' || OLD.volume || '"}');
END;

DROP TRIGGER IF EXISTS trg_jobs_audit_status;
CREATE TRIGGER trg_jobs_audit_status
AFTER UPDATE OF status ON jobs
FOR EACH ROW
WHEN OLD.status <> NEW.status
BEGIN
    INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata)
    VALUES ('JOB',
            CASE WHEN NEW.status = 'ABEND' THEN 'ERROR'
                 WHEN NEW.status = 'CANCELLED' THEN 'WARN'
                 ELSE 'INFO' END,
            'SYSTEM', NEW.job_name,
            'Job ' || NEW.job_name || ' ' || OLD.status || ' -> ' || NEW.status
                || ' RC=' || NEW.return_code,
            '{"action":"job-status","from":"' || OLD.status
                || '","to":"' || NEW.status
                || '","return_code":' || NEW.return_code || '}');
END;

DROP TRIGGER IF EXISTS trg_users_audit_status;
CREATE TRIGGER trg_users_audit_status
AFTER UPDATE OF status ON users
FOR EACH ROW
WHEN OLD.status <> NEW.status
BEGIN
    INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata)
    VALUES ('SECURITY',
            CASE WHEN NEW.status = 'LOCKED' THEN 'WARN' ELSE 'INFO' END,
            'SYSTEM', NEW.username,
            'Account status ' || OLD.status || ' -> ' || NEW.status,
            '{"action":"account-status","from":"' || OLD.status
                || '","to":"' || NEW.status || '"}');
END;

-- ===========================================================================
-- 5. parameter changes
-- ===========================================================================

DROP TRIGGER IF EXISTS trg_params_audit;
CREATE TRIGGER trg_params_audit
AFTER UPDATE ON system_parameters
FOR EACH ROW
WHEN OLD.param_value <> NEW.param_value
BEGIN
    UPDATE system_parameters SET updated_at = datetime('now')
     WHERE param_key = NEW.param_key;

    INSERT INTO audit_log (event_type, severity, actor, resource, message, metadata)
    VALUES ('CONFIG', 'INFO', 'SYSTEM', NEW.param_key,
            'Parameter ' || NEW.param_key || ' changed',
            '{"action":"param","from":"' || OLD.param_value
                || '","to":"' || NEW.param_value || '"}');
END;
