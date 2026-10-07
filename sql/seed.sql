-- ---------------------------------------------------------------------------
-- Mainframe Server System - seed data
-- Idempotent: safe to run repeatedly (INSERT OR IGNORE on unique keys).
-- ---------------------------------------------------------------------------

INSERT OR IGNORE INTO systems (name, sysplex, region, status, version) VALUES
    ('MAINFRAME-1', 'SYSPLEX-A', 'DEFAULT', 'ACTIVE',  '1.0.0'),
    ('MAINFRAME-2', 'SYSPLEX-A', 'BACKUP',  'ACTIVE',  '1.0.0'),
    ('MAINFRAME-DR','SYSPLEX-B', 'DR',      'OFFLINE', '1.0.0');

INSERT OR IGNORE INTO users
    (username, full_name, department, role, status, password_hash)
VALUES
    ('IBMUSER',  'System Administrator', 'SYSPROG',   'ADMIN',    'ACTIVE', 'sha256:seed'),
    ('OPERATOR1','Primary Operator',     'OPERATIONS','OPERATOR', 'ACTIVE', 'sha256:seed'),
    ('OPERATOR2','Secondary Operator',   'OPERATIONS','OPERATOR', 'ACTIVE', 'sha256:seed'),
    ('ANALYST1', 'Data Analyst',         'ANALYTICS', 'ANALYST',  'ACTIVE', 'sha256:seed'),
    ('GUEST',    'Guest Account',        'GENERAL',   'GUEST',    'ACTIVE', ''),
    ('BATCHUSR', 'Batch Service Account','SYSPROG',   'OPERATOR', 'ACTIVE', 'sha256:seed');

INSERT OR IGNORE INTO volumes (serial, device_type, capacity_mb, used_mb, status) VALUES
    ('MFVOL1', '3390-3', 2844,  1180, 'ONLINE'),
    ('MFVOL2', '3390-3', 2844,  2044, 'ONLINE'),
    ('MFVOL3', '3390-9', 8532,  3201, 'ONLINE'),
    ('MFTAPE1','3490E',  8192,  1200, 'RESERVED');

INSERT OR IGNORE INTO datasets
    (name, dsorg, recfm, lrecl, blksize, volume, owner_user_id, record_count, bytes_used, status)
VALUES
    ('MF1.PROD.CUSTOMER.MASTER', 'VSAM', 'FB', 200, 27920, 'MFVOL1', 1, 0, 0,  'AVAILABLE'),
    ('MF1.PROD.ACCOUNT.LEDGER',  'VSAM', 'FB', 180, 27920, 'MFVOL2', 1, 0, 0,  'AVAILABLE'),
    ('MF1.PROD.TRANSACTION.LOG', 'PS',   'VB', 256, 27920, 'MFVOL3', 2, 0, 0,  'AVAILABLE'),
    ('MF1.SYS.PARMLIB',          'PDS',  'FB',  80, 27920, 'MFVOL1', 1, 0, 0,  'AVAILABLE'),
    ('MF1.TEST.SAMPLE.DATA',     'PS',   'FB',  80, 27920, 'MFVOL2', 3, 0, 0,  'AVAILABLE'),
    ('MF1.ARCHIVE.2024',         'PS',   'FB', 133, 27920, 'MFTAPE1',1, 0, 0,  'MIGRATED');

-- The record_count / bytes_used columns are intentionally seeded as 0 rather
-- than hardcoded: the counters are derived data (see sql/triggers.sql) and are
-- reconciled from dataset_records by the final block below. Hardcoding them
-- would mean two sources of truth that silently disagree after a re-seed.
INSERT OR IGNORE INTO dataset_records (dataset_id, sequence, payload) VALUES
    (1, 1, 'CUST0001|ACME INDUSTRIES|ACTIVE|CREDIT-LIMIT-250000'),
    (1, 2, 'CUST0002|GLOBEX CORPORATION|ACTIVE|CREDIT-LIMIT-1000000'),
    (1, 3, 'CUST0003|INITECH CORP|SUSPENDED|CREDIT-LIMIT-50000'),
    (1, 4, 'CUST0004|UMBRELLA LOGISTICS|ACTIVE|CREDIT-LIMIT-750000'),
    (2, 1, 'ACCT-1000|CUST0001|DEBIT|12500.00|2024-01-15'),
    (2, 2, 'ACCT-1001|CUST0002|CREDIT|90000.00|2024-01-16'),
    (2, 3, 'ACCT-1002|CUST0003|DEBIT|3200.50|2024-01-17'),
    (3, 1, 'TXN-20240115-0001|ACCT-1000|POSTED|12500.00'),
    (3, 2, 'TXN-20240116-0002|ACCT-1001|POSTED|90000.00'),
    (3, 3, 'TXN-20240117-0003|ACCT-1002|REJECTED|3200.50'),
    (4, 1, 'SCHEDULER=JES2,MAXJOBS=8,JOBCLASS=A,B,C,D,E'),
    (4, 2, 'SECURITY=RACF,SESSION-TTL=30,SESSION-MAX=5'),
    (5, 1, 'SAMPLE RECORD ONE'),
    (5, 2, 'SAMPLE RECORD TWO'),
    (6, 1, 'ARCHIVED DATASET - MIGRATED TO TAPE MFTAPE1');

INSERT OR IGNORE INTO jobs
    (job_name, job_class, step_name, program, submitted_by, status, return_code, priority, cpu_ms, output)
VALUES
    ('DAILYPOST', 'A', 'STEP010', 'ACCTPOST', 1, 'COMPLETE', 0, 8, 4120, 'JOB DAILYPOST COMPLETED - 3 ACCOUNTS POSTED'),
    ('BACKUP01',  'B', 'STEP020', 'IEBCOPY',  1, 'COMPLETE', 0, 5, 8800, 'DATASET BACKUP COMPLETE'),
    ('REPORTS',   'C', 'STEP030', 'RPTPRINT', 2, 'RUNNING',  0, 4,  900, 'GENERATING MONTHLY REPORT'),
    ('CLEANUP',   'D', 'STEP040', 'IEFBR14',  2, 'QUEUED',   0, 3,    0, ''),
    ('BADLOAD',   'A', 'STEP050', 'LOADDATA', 3, 'ABEND',    8, 6, 1200, 'IEC141I 013-18 - DATASET NOT FOUND');

INSERT OR IGNORE INTO transactions
    (txn_code, terminal, user_id, status, rows_read, rows_written, elapsed_ms, detail)
VALUES
    ('CEMT', 'L3270', 1, 'OK',   0,  0,  12,  'INQUIRE SYSTEM'),
    ('INQ1', 'L3270', 2, 'OK',   4,  0,  45,  'CUSTOMER MASTER INQUIRY'),
    ('UPDT', 'L3271', 1, 'OK',   1,  1,  78,  'ACCOUNT LEDGER UPDATE'),
    ('INQ1', 'L3272', 3, 'WARN', 0,  0,  22,  'NO RECORDS MATCHED'),
    ('POST', 'L3270', 1, 'FAIL', 2,  0, 130,  'BALANCE CHECK FAILED'),
    ('CEMT', 'L3273', 4, 'OK',   0,  0,   9,  'INQUIRE JOB QUEUE');

INSERT OR IGNORE INTO audit_log (event_type, severity, actor, resource, message, metadata) VALUES
    ('LOGON',   'INFO',  'IBMUSER',   'SYSTEM',   'Operator logon accepted',        '{"terminal":"L3270"}'),
    ('COMMAND', 'INFO',  'IBMUSER',   'JES2',     'Issued $D J,LIST',               '{"jobs":5}'),
    ('DB',      'INFO',  'SYSTEM',    'datasets', 'Catalog scan completed',         '{"datasets":6}'),
    ('CONFIG',  'WARN',  'SYSTEM',    'http',     'API key authentication disabled', '{"authRequired":false}'),
    ('LOGON',   'WARN',  'UNKNOWN',   'SYSTEM',   'Logon rejected - unknown user',  '{"attempts":3}');

INSERT OR IGNORE INTO system_parameters (param_key, param_value, description) VALUES
    ('JES2.MAXJOBS',        '8',    'Maximum concurrent batch jobs'),
    ('JES2.JOBCLASS',       'A,B,C,D,E', 'Active job classes'),
    ('RACF.SESSION.TTL',    '30',   'Session time-to-live in minutes'),
    ('SMF.RECORDING',       'ON',   'System management facility recording'),
    ('STORAGE.DEFAULT.VOL', 'MFVOL1','Default volume for new datasets'),
    ('TCP.BANNER',          'WELCOME TO MAINFRAME-1 - LOGON REQUIRED', 'Terminal banner');

-- ---------------------------------------------------------------------------
-- Reconcile derived counters.
--
-- dataset_records was populated above, but the triggers that keep
-- datasets.record_count / bytes_used in step only exist from revision 005.
-- On a fresh database the seed (002) runs before them, so the counters are
-- recomputed here from the records themselves. This is idempotent: running
-- the seed again recomputes the same values.
-- ---------------------------------------------------------------------------
UPDATE datasets
   SET record_count = COALESCE((
           SELECT COUNT(*) FROM dataset_records r
            WHERE r.dataset_id = datasets.dataset_id), 0),
       bytes_used = COALESCE((
           SELECT SUM(LENGTH(r.payload)) FROM dataset_records r
            WHERE r.dataset_id = datasets.dataset_id), 0);

-- and the volumes, from the datasets placed on them (in MiB, as the
-- volume-usage trigger does).
UPDATE volumes
   SET used_mb = COALESCE((
           SELECT SUM(d.bytes_used) / 1048576 FROM datasets d
            WHERE d.volume = volumes.serial AND d.status <> 'DELETED'), 0);
