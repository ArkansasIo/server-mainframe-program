-- ---------------------------------------------------------------------------
-- Mainframe Server System - migration tracking
--
-- The migrator (src/db/migrations.cpp, scripts/db-init.js, src/db/migrate.js)
-- owns the schema_migrations table and records one row per applied revision.
-- This file defines the table explicitly so a database can be created from the
-- SQL alone, without the C++ tool.
--
-- NOTE: this file deliberately does NOT pre-insert the revision rows. Doing so
-- would mark every revision as applied before the migrator had run it, and the
-- migrator would then skip the schema/seed/views files entirely. The migrator
-- inserts each row as it applies that revision.
--
-- Revisions applied by the migrator, in order:
--   001_schema    sql/schema.sql      tables and core indexes
--   002_seed      sql/seed.sql        demonstration data
--   003_views     sql/views.sql       reporting views
--   004_indexes   sql/indexes.sql     performance indexes
--   005_triggers  sql/triggers.sql    audit and integrity triggers
--   006_views_ops sql/views_ops.sql   operational reporting views
-- ---------------------------------------------------------------------------

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS schema_migrations (
    revision    TEXT PRIMARY KEY,
    applied_at  TEXT NOT NULL DEFAULT (datetime('now')),
    description TEXT NOT NULL DEFAULT ''
);

-- A view so an operator can see the migration state without remembering SQL.
DROP VIEW IF EXISTS v_schema_state;
CREATE VIEW v_schema_state AS
SELECT
    revision                                        AS revision,
    applied_at                                      AS applied_at,
    description                                     AS description
FROM schema_migrations
ORDER BY revision;
