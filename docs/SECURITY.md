# Security and deployment checklist

This checklist records deployment-sensitive settings that should be reviewed before exposing the mainframe server to an untrusted network.

## HTTP control plane

- Set `MF_HTTP_AUTH_REQUIRED=true` for deployed environments.
- Supply a unique secret through `MF_API_KEYS`; do not use the checked-in development key.
- Keep the API behind a trusted reverse proxy or private network until authentication, CORS, and route access controls have been reviewed.
- Do not use `origins: ["*"]` for credentialed browser deployments. Configure an explicit origin allowlist.
- Expose only the HTTP port required by the operator console and API.

## TCP services

- Terminal and dataset transfer ports bind to the configured host. Restrict them with a firewall or private network unless remote access is intentional.
- Treat the dataset transfer service as a privileged interface. Validate authentication and authorization for every operation before public exposure.
- Use TLS termination or a secure network tunnel when services cross untrusted networks.

## Data and credentials

- Keep `config/local.json`, `.env` files, databases, backups, logs, and uploaded datasets out of version control.
- Rotate any secret that has ever been committed or shared.
- Back up the database and test restoring a backup; protect backup files with OS-level permissions.
- Review seed data before production use and disable automatic seeding where it is not appropriate.

## Validation before release

- Run `npm ci`, `npm test`, and `npm run db:check-queries`.
- Build the C++ project with CMake and run CTest.
- Verify expected authentication behavior, rejected unauthenticated requests, and CORS allowlist behavior with integration tests.
- Confirm logs do not include API keys, passwords, session tokens, or sensitive dataset contents.
- Use a non-privileged service account and restrict filesystem permissions.

This is an operational checklist, not a security certification. Validate behavior against the exact configuration and deployment environment.
