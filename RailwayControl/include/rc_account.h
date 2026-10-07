/* ==========================================================================
 * rc_account.h - 👤 Accounts, authentication and permissions.
 *
 * RailControl models the access control a real signalling control centre
 * needs. It is a straightforward RBAC (role based access control) scheme:
 *
 *   ACCOUNT   a named user with a password hash, a role and a status
 *   ROLE      a named set of permissions
 *   PERMISSION an atomic capability (SET_SIGNAL, REQUEST_ROUTE, ...)
 *
 * Separating permissions from roles matters: a real interlocking must be able
 * to prove, from a table, exactly which capability each operator holds. The
 * permission matrix in this file is that table.
 *
 * Safety-critical design points
 * -----------------------------
 *  - The safety-critical actions are PERMISSION gated, not role gated, so an
 *    operator can be given, say, route setting without emergency authority.
 *  - An account is LOCKED after too many failed logons, and only a user with
 *    PERM_MANAGE_ACCOUNTS may unlock it.
 *  - Every logon, logoff, refusal and permission change is written to the
 *    authentication log, which is separate from the operational event log so
 *    it can be retained for longer.
 *  - Passwords are never stored in clear. The hash is salted and iterated;
 *    see rc_account_hash() for the scheme and its deliberate limitations.
 *
 * SAFETY NOTICE
 * -------------
 * This is prototype access control for a simulator. A deployable system needs
 * hardware tokens, dual control for safety actions, tamper-evident audit and
 * compliance with the operator's own security policy.
 * ========================================================================== */
#ifndef RC_ACCOUNT_H
#define RC_ACCOUNT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RC_MAX_ACCOUNTS 64
#define RC_MAX_USERNAME 32
#define RC_MAX_PASSWORD 64
#define RC_MAX_FULLNAME 48
#define RC_AUTH_LOG_SIZE 512
#define RC_MAX_SESSIONS 32

    /* --------------------------------------------------------------------------
     * Roles, ordered by seniority so they can be compared.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        ROLE_VIEWER = 0, /* read the diagram and the logs only */
        ROLE_SIGNALLER,  /* work signals, points and routes */
        ROLE_SUPERVISOR, /* signaller plus emergency and overrides */
        ROLE_ADMIN       /* everything, including accounts and settings */
    } RcRole;

    const char *rc_role_name(RcRole role);
    const char *rc_role_description(RcRole role);
    RcRole rc_role_parse(const char *name);

    /* --------------------------------------------------------------------------
     * Permissions. Each is a single capability.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        PERM_VIEW_DIAGRAM = 0,   /* see the track diagram and status */
        PERM_VIEW_LOGS,          /* read the event and auth logs */
        PERM_SET_SIGNAL,         /* work a signal */
        PERM_SET_POINT,          /* work a point */
        PERM_RELEASE_POINT,      /* manual release of a route lock */
        PERM_REQUEST_ROUTE,      /* set / cancel a route */
        PERM_DISPATCH_TRAIN,     /* give a train authority */
        PERM_CONTROL_TRAIN,      /* hold / speed a train */
        PERM_INJECT_SENSOR,      /* simulate a field input */
        PERM_FAIL_TRACK_CIRCUIT, /* withdraw a track circuit */
        PERM_EMERGENCY_STOP,     /* trip the emergency stop */
        PERM_CLEAR_EMERGENCY,    /* cancel the emergency stop */
        PERM_ACKNOWLEDGE_ALARM,  /* acknowledge alarms */
        PERM_MANAGE_JOBS,        /* enable / trigger CONJOB rules */
        PERM_RUN_SCRIPTS,        /* run Lua */
        PERM_CHANGE_SETTINGS,    /* change runtime settings */
        PERM_VIEW_FINANCIAL,     /* (reserved) cost / performance figures */
    PERM_MANAGE_ACCOUNTS,    /* create, disable, unlock, change roles */
    PERM_SHUTDOWN,           /* stop the program */

    /* Keep last - used to size the permission table. */
    PERM_COUNT
} RcPermission;

    const char *rc_permission_name(RcPermission permission);
    const char *rc_permission_description(RcPermission permission);
    RcPermission rc_permission_parse(const char *name);

    /* Does `role` hold `permission` by default? This is the static matrix. */
    bool rc_role_has(RcRole role, RcPermission permission);

    /* The full matrix, for the GUI permissions viewer. Row = role, column = perm. */
    const bool *rc_permission_matrix(void);

    /* --------------------------------------------------------------------------
     * Accounts
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        ACCOUNT_ACTIVE = 0,
        ACCOUNT_LOCKED,     /* too many failed logons */
        ACCOUNT_DISABLED,   /* administratively withdrawn */
        ACCOUNT_MUST_CHANGE /* password must be changed at next logon */
    } RcAccountStatus;

    const char *rc_account_status_name(RcAccountStatus status);

    typedef struct
    {
        int id;
        char username[RC_MAX_USERNAME];
        char full_name[RC_MAX_FULLNAME];
        RcRole role;
        RcAccountStatus status;
        int failed_logons;
        unsigned long last_logon_ms;
        unsigned long created_ms;
        unsigned long password_changed_ms;
        int logon_count;
        char last_address[48];
        /* Extra permissions granted on top of the role, and revoked from it. */
        unsigned long granted_extra; /* bitmask over RcPermission */
        unsigned long revoked;       /* bitmask over RcPermission */
    } RcAccount;

    /* --------------------------------------------------------------------------
     * Sessions
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        int id;
        int account_id;
        char username[RC_MAX_USERNAME];
        RcRole role;
        char terminal[32]; /* "GUI", "CONSOLE", "REMOTE" */
        unsigned long started_ms;
        unsigned long last_seen_ms;
        unsigned long expires_ms;
        int actions_performed;
        bool in_use;
    } RcSession;

    /* --------------------------------------------------------------------------
     * Lifecycle
     * -------------------------------------------------------------------------- */

    /* Create the account store and install the default accounts.
     * Default accounts are documented in rc_account.c and every one of them has a
     * password that MUST be changed before any real use. */
    void rc_account_init(void);
    void rc_account_shutdown(void);

    /* --------------------------------------------------------------------------
     * Logon / logoff
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        AUTH_OK = 0,
        AUTH_ERR_UNKNOWN_USER,
        AUTH_ERR_BAD_PASSWORD,
        AUTH_ERR_LOCKED,
        AUTH_ERR_DISABLED,
        AUTH_ERR_NO_SESSION,
        AUTH_ERR_EXPIRED,
        AUTH_ERR_NOT_PERMITTED
    } RcAuthResult;

    const char *rc_auth_result_name(RcAuthResult result);
    const char *rc_auth_result_message(RcAuthResult result);

    /* Log on. On success the new session id (>0) is returned through `session_id`.
     * Every attempt, successful or not, is written to the auth log. */
    RcAuthResult rc_logon(const char *username, const char *password,
                          const char *terminal, const char *address,
                          int *session_id);

    /* Log off, destroying the session. */
    RcAuthResult rc_logoff(int session_id);

    /* Locate and validate a session. `permission` is checked when it is >= 0.
     * Expired sessions are destroyed and reported as AUTH_ERR_EXPIRED. */
    RcAuthResult rc_session_check(int session_id, RcPermission permission);

    /* Refresh a session's idle timer (call on each operator action). */
    RcAuthResult rc_session_touch(int session_id);

    /* Convenience: does this session hold the permission? */
    bool rc_session_can(int session_id, RcPermission permission);

    /* The account behind a session, or NULL. */
    const RcAccount *rc_session_account(int session_id);
    RcRole rc_session_role(int session_id);

    int rc_session_count(void);
    bool rc_session_get_at(int index, RcSession *out);

    /* --------------------------------------------------------------------------
     * The operator currently signed in (single-operator front ends)
     * --------------------------------------------------------------------------
     * The GUI and the console track one active session. These helpers let the
     * front end ask "can the signed-in operator do this?" without threading a
     * session id through every call. */
    void rc_set_current_session(int session_id);
    int rc_current_session(void);
    bool rc_can(RcPermission permission);     /* the active session */
    bool rc_require(RcPermission permission); /* logs a refusal when it fails */

    /* --------------------------------------------------------------------------
     * Account administration (requires PERM_MANAGE_ACCOUNTS)
     * -------------------------------------------------------------------------- */

    /* Create an account. Returns AUTH_OK, or an RcAuthResult describing the
     * refusal. The new account id is written through `new_account_id` when that
     * pointer is non-NULL. */
    RcAuthResult rc_account_create(int session_id, const char *username, const char *password,
                                   const char *full_name, RcRole role, int *new_account_id);

    /* Remove an account. Refused for the last administrator and for self. */
    RcAuthResult rc_account_remove(int session_id, int account_id);

    RcAuthResult rc_account_set_role(int session_id, int account_id, RcRole role);
    RcAuthResult rc_account_set_status(int session_id, int account_id, RcAccountStatus status);
    RcAuthResult rc_account_unlock(int session_id, int account_id);

    /* Change a password. A user may change their own; an administrator may change
     * anyone's. The old password is required unless the caller is an admin. */
    RcAuthResult rc_account_change_password(int session_id, int account_id,
                                            const char *old_password,
                                            const char *new_password);

    /* Grant or revoke a single permission on top of the role. */
    RcAuthResult rc_account_grant(int session_id, int account_id, RcPermission permission);
    RcAuthResult rc_account_revoke(int session_id, int account_id, RcPermission permission);

    int rc_account_count(void);
    const RcAccount *rc_account_get_at(int index);
    const RcAccount *rc_account_find(const char *username);
    const RcAccount *rc_account_by_id(int account_id);

    /* Effective permissions: the role matrix, plus grants, minus revocations. */
    unsigned long rc_account_permissions(const RcAccount *account);
    bool rc_account_can(const RcAccount *account, RcPermission permission);

    /* --------------------------------------------------------------------------
     * Password policy
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        int min_length;
        bool require_upper;
        bool require_lower;
        bool require_digit;
        bool require_symbol;
        int max_failed_logons;
        int session_timeout_minutes;
        bool must_change_default; /* force a change of installed defaults */
    } RcPasswordPolicy;

    RcPasswordPolicy *rc_password_policy(void);

    /* Returns NULL when acceptable, otherwise a static reason string. */
    const char *rc_password_validate(const char *password);

    /* --------------------------------------------------------------------------
     * Registration-time validation
     *
     * Account creation is the one place an operator supplies both the username
     * and the password by hand, so it is the one place both need checking
     * before anything is written:
     *
     *   username   must satisfy a syntax rule, not just be non-empty
     *   password   must satisfy the policy AND be typed twice
     *
     * Keeping these here rather than in a front end means the console, the GUI
     * and any future web form all enforce the same rules.
     * -------------------------------------------------------------------------- */

    /* Username syntax: 3..RC_MAX_USERNAME characters, must start with a
     * letter, then letters, digits, dot, underscore or hyphen.
     * Returns NULL when acceptable, otherwise a static reason string. */
    const char *rc_username_validate(const char *username);

    /* Check a password and its confirmation match. `confirmation` may be NULL
     * to skip the match check (used where a caller has only one field).
     * Returns NULL when acceptable, otherwise a static reason string. */
    const char *rc_password_confirm_validate(const char *password,
                                             const char *confirmation);

    /* --------------------------------------------------------------------------
     * Authentication log  (separate from the operational event log)
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        AUTHLOG_LOGON_OK = 0,
        AUTHLOG_LOGON_FAIL,
        AUTHLOG_LOGOFF,
        AUTHLOG_TIMEOUT,
        AUTHLOG_LOCKED,
        AUTHLOG_PERMISSION_DENIED,
        AUTHLOG_ACCOUNT_CHANGE,
        AUTHLOG_SYSTEM
    } RcAuthLogKind;

    typedef struct
    {
        unsigned long id;
        RcAuthLogKind kind;
        char timestamp[24];
        char username[RC_MAX_USERNAME];
        char detail[160];
        char address[48];
    } RcAuthLogEntry;

    const char *rc_authlog_kind_name(RcAuthLogKind kind);

    /* Append an entry. The log is a bounded ring; the oldest entry is dropped. */
    void rc_authlog_write(RcAuthLogKind kind, const char *username,
                          const char *address, const char *detail);

    int rc_authlog_count(void);
    bool rc_authlog_get_at(int index_from_newest, RcAuthLogEntry *out);
    int rc_authlog_clear(void);

    /* Render the log into a text buffer (for the GUI pane and the CLI). */
    void rc_authlog_format(int count, char *buffer, size_t size);

    /* Physical password hash for an account, exposed only so the loader can
     * persist it. Never display this. */
    void rc_account_hash_password(RcAccount *account, const char *password);
    bool rc_account_verify_password(const RcAccount *account, const char *password);

    /* Persistence: write / read accounts (without hashes to stdout). */
    int rc_account_save(const char *path);
    int rc_account_load(const char *path);
    int rc_account_export_redacted(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* RC_ACCOUNT_H */
