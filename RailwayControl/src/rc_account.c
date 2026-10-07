/* ==========================================================================
 * rc_account.c - 👤 Accounts, sessions, permissions and the auth log.
 *
 * Password hashing
 * ----------------
 * A salted, iterated FNV-1a based construction is used. This is NOT a
 * cryptographically strong KDF and is chosen only because the standard
 * library is all that is available here without adding a dependency.
 *
 * A real control system must use a memory-hard KDF (scrypt, Argon2id or
 * PBKDF2-HMAC-SHA256 with a high iteration count) from a vetted crypto
 * library, plus hardware tokens for the safety-critical roles. That
 * limitation is called out in the safety notice printed by CREDITS.
 * ========================================================================== */
#include "rc_account.h"
#include "rc_version.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ==========================================================================
 * Permission naming
 * ========================================================================== */
static const char *g_permission_names[PERM_COUNT] = {
    "VIEW_DIAGRAM",
    "VIEW_LOGS",
    "SET_SIGNAL",
    "SET_POINT",
    "RELEASE_POINT",
    "REQUEST_ROUTE",
    "DISPATCH_TRAIN",
    "CONTROL_TRAIN",
    "INJECT_SENSOR",
    "FAIL_TRACK_CIRCUIT",
    "EMERGENCY_STOP",
    "CLEAR_EMERGENCY",
    "ACKNOWLEDGE_ALARM",
    "MANAGE_JOBS",
    "RUN_SCRIPTS",
    "CHANGE_SETTINGS",
    "VIEW_FINANCIAL",
    "MANAGE_ACCOUNTS",
    "SHUTDOWN"};

static const char *g_permission_descriptions[PERM_COUNT] = {
    "View the track diagram and system status",
    "Read the event log and the authentication log",
    "Operate signals (clear and replace)",
    "Operate points / switches",
    "Release a point from a route lock",
    "Set and cancel routes",
    "Give a train a movement authority",
    "Hold, release and set the speed of a train",
    "Inject simulated field sensor readings",
    "Withdraw a track circuit (declare a section unproven)",
    "Trip the emergency stop",
    "Clear the emergency stop",
    "Acknowledge alarms",
    "Enable, disable and trigger automated control jobs",
    "Run Lua scripts against the control API",
    "Change runtime settings",
    "View cost and performance figures",
    "Create, disable, unlock accounts and change roles",
    "Shut the control system down"};

const char *rc_permission_name(RcPermission permission)
{
    if (permission < 0 || permission >= PERM_COUNT)
    {
        return "UNKNOWN";
    }
    return g_permission_names[permission];
}

const char *rc_permission_description(RcPermission permission)
{
    if (permission < 0 || permission >= PERM_COUNT)
    {
        return "";
    }
    return g_permission_descriptions[permission];
}

RcPermission rc_permission_parse(const char *name)
{
    int i;

    if (name == NULL)
    {
        return (RcPermission)-1;
    }
    for (i = 0; i < PERM_COUNT; ++i)
    {
        if (strcmp(g_permission_names[i], name) == 0)
        {
            return (RcPermission)i;
        }
    }
    return (RcPermission)-1;
}

/* ==========================================================================
 * Role and permission matrix
 *
 * Every cell is stated explicitly rather than computed, because this table is
 * the security policy: it should be readable and reviewable as a table.
 * ========================================================================== */
static const bool g_matrix[4][PERM_COUNT] = {
    /* ROLE_VIEWER: a read-only observer. Can see the diagram and the logs,
     * acknowledge alarms so the screen can be cleared, and nothing else. */
    {
        true,  /* VIEW_DIAGRAM           */
        true,  /* VIEW_LOGS              */
        false, /* SET_SIGNAL             */
        false, /* SET_POINT              */
        false, /* RELEASE_POINT          */
        false, /* REQUEST_ROUTE          */
        false, /* DISPATCH_TRAIN         */
        false, /* CONTROL_TRAIN          */
        false, /* INJECT_SENSOR          */
        false, /* FAIL_TRACK_CIRCUIT     */
        false, /* EMERGENCY_STOP         */
        false, /* CLEAR_EMERGENCY        */
        true,  /* ACKNOWLEDGE_ALARM      */
        false, /* MANAGE_JOBS            */
        false, /* RUN_SCRIPTS            */
        false, /* CHANGE_SETTINGS        */
        false, /* VIEW_FINANCIAL         */
        false, /* MANAGE_ACCOUNTS        */
        false  /* SHUTDOWN               */
    },
    /* ROLE_SIGNALLER: the normal working role. Works signals, points, routes
     * and trains. Deliberately does NOT hold the emergency permissions or any
     * administrative capability. */
    {
        true,  /* VIEW_DIAGRAM           */
        true,  /* VIEW_LOGS              */
        true,  /* SET_SIGNAL             */
        true,  /* SET_POINT              */
        false, /* RELEASE_POINT          */
        true,  /* REQUEST_ROUTE          */
        true,  /* DISPATCH_TRAIN         */
        true,  /* CONTROL_TRAIN          */
        true,  /* INJECT_SENSOR          */
        false, /* FAIL_TRACK_CIRCUIT     */
        false, /* EMERGENCY_STOP         */
        false, /* CLEAR_EMERGENCY        */
        true,  /* ACKNOWLEDGE_ALARM      */
        true,  /* MANAGE_JOBS            */
        false, /* RUN_SCRIPTS            */
        false, /* CHANGE_SETTINGS        */
        false, /* VIEW_FINANCIAL         */
        false, /* MANAGE_ACCOUNTS        */
        false  /* SHUTDOWN               */
    },
    /* ROLE_SUPERVISOR: the signaller plus every safety and override action.
     * Holds both emergency permissions - in a real interlocking these would
     * additionally require dual control. */
    {
        true,  /* VIEW_DIAGRAM           */
        true,  /* VIEW_LOGS              */
        true,  /* SET_SIGNAL             */
        true,  /* SET_POINT              */
        true,  /* RELEASE_POINT          */
        true,  /* REQUEST_ROUTE          */
        true,  /* DISPATCH_TRAIN         */
        true,  /* CONTROL_TRAIN          */
        true,  /* INJECT_SENSOR          */
        true,  /* FAIL_TRACK_CIRCUIT     */
        true,  /* EMERGENCY_STOP         */
        true,  /* CLEAR_EMERGENCY        */
        true,  /* ACKNOWLEDGE_ALARM      */
        true,  /* MANAGE_JOBS            */
        true,  /* RUN_SCRIPTS            */
        true,  /* CHANGE_SETTINGS        */
        true,  /* VIEW_FINANCIAL         */
        false, /* MANAGE_ACCOUNTS        */
        false  /* SHUTDOWN               */
    },
    /* ROLE_ADMIN: everything, including accounts and shutdown. */
    {
        true, true, true, true, true, true, true, true, true, true,
        true, true, true, true, true, true, true, true, true}};

const bool *rc_permission_matrix(void)
{
    return &g_matrix[0][0];
}

bool rc_role_has(RcRole role, RcPermission permission)
{
    if (role < ROLE_VIEWER || role > ROLE_ADMIN)
    {
        return false;
    }
    if (permission < 0 || permission >= PERM_COUNT)
    {
        return false;
    }
    return g_matrix[(int)role][(int)permission];
}

const char *rc_role_name(RcRole role)
{
    switch (role)
    {
    case ROLE_VIEWER:
        return "VIEWER";
    case ROLE_SIGNALLER:
        return "SIGNALLER";
    case ROLE_SUPERVISOR:
        return "SUPERVISOR";
    case ROLE_ADMIN:
        return "ADMIN";
    }
    return "VIEWER";
}

const char *rc_role_description(RcRole role)
{
    switch (role)
    {
    case ROLE_VIEWER:
        return "Read-only observer";
    case ROLE_SIGNALLER:
        return "Normal working role: signals, points, routes, trains";
    case ROLE_SUPERVISOR:
        return "Signaller plus emergency and override authority";
    case ROLE_ADMIN:
        return "Full control including accounts and settings";
    }
    return "";
}

RcRole rc_role_parse(const char *name)
{
    int i;

    if (name == NULL)
    {
        return ROLE_VIEWER;
    }
    for (i = ROLE_VIEWER; i <= ROLE_ADMIN; ++i)
    {
        const char *candidate = rc_role_name((RcRole)i);
        size_t j;
        bool match = true;
        for (j = 0; candidate[j] != '\0' && name[j] != '\0'; ++j)
        {
            if (tolower((unsigned char)candidate[j]) != tolower((unsigned char)name[j]))
            {
                match = false;
                break;
            }
        }
        if (match && candidate[j] == '\0' && name[j] == '\0')
        {
            return (RcRole)i;
        }
    }
    return ROLE_VIEWER;
}

const char *rc_account_status_name(RcAccountStatus status)
{
    switch (status)
    {
    case ACCOUNT_ACTIVE:
        return "ACTIVE";
    case ACCOUNT_LOCKED:
        return "LOCKED";
    case ACCOUNT_DISABLED:
        return "DISABLED";
    case ACCOUNT_MUST_CHANGE:
        return "MUST-CHANGE";
    }
    return "ACTIVE";
}

/* ==========================================================================
 * Password hashing
 * ========================================================================== */
#define RC_HASH_SALT_LEN 16
#define RC_HASH_ITERATIONS 4096
#define RC_HASH_LEN 32

static void hash_bytes(const char *salt, size_t salt_len,
                       const char *password, size_t password_len,
                       unsigned char *out, size_t out_len)
{
    /* FNV-1a expanded to `out_len` bytes. Iterated to slow a single guess.
     * See the file header: this is NOT a strong KDF. */
    size_t i;
    size_t k;

    for (i = 0; i < out_len; ++i)
    {
        unsigned long long hash = 1469598103934665603ULL;
        int round;

        hash ^= (unsigned char)salt[i % salt_len];
        hash *= 1099511628211ULL;
        for (k = 0; k < password_len; ++k)
        {
            hash ^= (unsigned char)password[k];
            hash *= 1099511628211ULL;
        }
        hash ^= (unsigned long long)(i * 2654435761u);
        hash *= 1099511628211ULL;

        for (round = 0; round < RC_HASH_ITERATIONS; ++round)
        {
            hash ^= (hash >> 33);
            hash *= 0xff51afd7ed558ccdULL;
            hash ^= (hash >> 29);
        }
        out[i] = (unsigned char)(hash & 0xFFu);
    }
}

static void bytes_to_hex(const unsigned char *bytes, size_t length, char *out)
{
    static const char *digits = "0123456789abcdef";
    size_t i;

    for (i = 0; i < length; ++i)
    {
        out[i * 2] = digits[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[length * 2] = '\0';
}

/* Account storage. password_hash is deliberately not part of the public
 * RcAccount view; it lives in a parallel array indexed by slot. */
typedef struct
{
    char salt[RC_HASH_SALT_LEN * 2 + 1];
    char hash[RC_HASH_LEN * 2 + 1];
} PasswordRecord;

static RcAccount g_accounts[RC_MAX_ACCOUNTS];
static PasswordRecord g_passwords[RC_MAX_ACCOUNTS];
static int g_account_count = 0;
static RcSession g_sessions[RC_MAX_SESSIONS];
static int g_current_session = 0;
static int g_next_account_id = 1;
static int g_next_session_id = 1;
static RcPasswordPolicy g_policy;

/* Authentication log ring. */
static RcAuthLogEntry g_authlog[RC_AUTH_LOG_SIZE];
static int g_authlog_count = 0;
static int g_authlog_next = 0;
static unsigned long g_authlog_next_id = 1;

/* --------------------------------------------------------------------------
 * Time helpers
 * -------------------------------------------------------------------------- */
static unsigned long now_ms(void)
{
    return (unsigned long)((unsigned long long)time(NULL) * 1000ULL);
}

static void now_string(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_buf;

#if defined(_WIN32)
    if (gmtime_s(&tm_buf, &now) != 0)
    {
        snprintf(buffer, size, "0000-00-00 00:00:00");
        return;
    }
#else
    if (gmtime_r(&now, &tm_buf) == NULL)
    {
        snprintf(buffer, size, "0000-00-00 00:00:00");
        return;
    }
#endif
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
}

/* --------------------------------------------------------------------------
 * Password policy
 * -------------------------------------------------------------------------- */
RcPasswordPolicy *rc_password_policy(void)
{
    return &g_policy;
}

const char *rc_password_validate(const char *password)
{
    size_t length;
    size_t i;
    bool has_upper = false;
    bool has_lower = false;
    bool has_digit = false;
    bool has_symbol = false;

    if (password == NULL)
    {
        return "Password must not be empty";
    }
    length = strlen(password);
    if ((int)length < g_policy.min_length)
    {
        return "Password is shorter than the minimum length";
    }
    if ((int)length > RC_MAX_PASSWORD)
    {
        return "Password is longer than the maximum length";
    }

    for (i = 0; i < length; ++i)
    {
        const unsigned char c = (unsigned char)password[i];
        if (isupper(c))
        {
            has_upper = true;
        }
        else if (islower(c))
        {
            has_lower = true;
        }
        else if (isdigit(c))
        {
            has_digit = true;
        }
        else
        {
            has_symbol = true;
        }
    }

    if (g_policy.require_upper && !has_upper)
    {
        return "Password must contain an uppercase letter";
    }
    if (g_policy.require_lower && !has_lower)
    {
        return "Password must contain a lowercase letter";
    }
    if (g_policy.require_digit && !has_digit)
    {
        return "Password must contain a digit";
    }
    if (g_policy.require_symbol && !has_symbol)
    {
        return "Password must contain a symbol";
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Registration-time validation
 * -------------------------------------------------------------------------- */
const char *rc_username_validate(const char *username)
{
    size_t length;
    size_t i;

    if (username == NULL || username[0] == '\0')
    {
        return "Userid must not be empty";
    }

    length = strlen(username);
    if (length < 3)
    {
        return "Userid must be at least 3 characters";
    }
    if (length > RC_MAX_USERNAME)
    {
        return "Userid is too long";
    }

    /* Must start with a letter. A leading digit would be confusing at a
     * console where numeric replies also select a menu item. */
    if (!isalpha((unsigned char)username[0]))
    {
        return "Userid must start with a letter";
    }

    for (i = 0; i < length; ++i)
    {
        const unsigned char c = (unsigned char)username[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-')
        {
            return "Userid may contain only letters, digits, dot, underscore and hyphen";
        }
    }
    return NULL;
}

const char *rc_password_confirm_validate(const char *password,
                                         const char *confirmation)
{
    const char *reason = rc_password_validate(password);

    if (reason != NULL)
    {
        return reason;
    }
    if (confirmation == NULL)
    {
        return NULL;
    }
    if (strcmp(password, confirmation) != 0)
    {
        return "Passwords do not match";
    }
    return NULL;
}

void rc_account_hash_password(RcAccount *account, const char *password)
{
    int slot;
    unsigned char raw[RC_HASH_LEN];

    if (account == NULL || password == NULL)
    {
        return;
    }
    slot = account->id - 1;
    if (slot < 0 || slot >= RC_MAX_ACCOUNTS)
    {
        return;
    }

    /* Salt from the username, the account id and the clock, so two accounts
     * with the same password never share a hash. */
    snprintf(g_passwords[slot].salt, sizeof(g_passwords[slot].salt),
             "%08x%08x%08x%08x",
             (unsigned)(account->id * 2654435761u),
             (unsigned)now_ms(),
             (unsigned)strlen(account->username),
             (unsigned)(now_ms() >> 16));

    hash_bytes(g_passwords[slot].salt, strlen(g_passwords[slot].salt),
               password, strlen(password), raw, sizeof(raw));
    bytes_to_hex(raw, sizeof(raw), g_passwords[slot].hash);

    account->password_changed_ms = now_ms();
}

bool rc_account_verify_password(const RcAccount *account, const char *password)
{
    int slot;
    unsigned char raw[RC_HASH_LEN];
    char computed[RC_HASH_LEN * 2 + 1];
    size_t i;
    unsigned char diff = 0;

    if (account == NULL || password == NULL)
    {
        return false;
    }
    slot = account->id - 1;
    if (slot < 0 || slot >= RC_MAX_ACCOUNTS)
    {
        return false;
    }
    if (g_passwords[slot].hash[0] == '\0')
    {
        return false; /* account has no password set */
    }

    hash_bytes(g_passwords[slot].salt, strlen(g_passwords[slot].salt),
               password, strlen(password), raw, sizeof(raw));
    bytes_to_hex(raw, sizeof(raw), computed);

    /* Constant-time comparison: never let the compare time leak how much of
     * the password was right. */
    for (i = 0; i < RC_HASH_LEN * 2; ++i)
    {
        diff = (unsigned char)(diff | (unsigned char)(computed[i] ^ g_passwords[slot].hash[i]));
    }
    return diff == 0;
}

/* ==========================================================================
 * Effective permissions
 * ========================================================================== */
unsigned long rc_account_permissions(const RcAccount *account)
{
    unsigned long mask = 0;
    int i;

    if (account == NULL)
    {
        return 0;
    }
    for (i = 0; i < PERM_COUNT; ++i)
    {
        bool held = rc_role_has(account->role, (RcPermission)i);
        if (account->granted_extra & (1UL << i))
        {
            held = true;
        }
        if (account->revoked & (1UL << i))
        {
            held = false;
        }
        if (held)
        {
            mask |= (1UL << i);
        }
    }
    return mask;
}

bool rc_account_can(const RcAccount *account, RcPermission permission)
{
    if (account == NULL || permission < 0 || permission >= PERM_COUNT)
    {
        return false;
    }
    if (account->status != ACCOUNT_ACTIVE && account->status != ACCOUNT_MUST_CHANGE)
    {
        return false;
    }
    return (rc_account_permissions(account) & (1UL << (int)permission)) != 0;
}

/* ==========================================================================
 * Authentication log
 * ========================================================================== */
const char *rc_authlog_kind_name(RcAuthLogKind kind)
{
    switch (kind)
    {
    case AUTHLOG_LOGON_OK:
        return "LOGON";
    case AUTHLOG_LOGON_FAIL:
        return "LOGON-FAIL";
    case AUTHLOG_LOGOFF:
        return "LOGOFF";
    case AUTHLOG_TIMEOUT:
        return "TIMEOUT";
    case AUTHLOG_LOCKED:
        return "LOCKED";
    case AUTHLOG_PERMISSION_DENIED:
        return "DENIED";
    case AUTHLOG_ACCOUNT_CHANGE:
        return "ACCOUNT";
    case AUTHLOG_SYSTEM:
        return "SYSTEM";
    }
    return "AUTH";
}

void rc_authlog_write(RcAuthLogKind kind, const char *username,
                      const char *address, const char *detail)
{
    RcAuthLogEntry *slot = &g_authlog[g_authlog_next];

    memset(slot, 0, sizeof(*slot));
    slot->id = g_authlog_next_id++;
    slot->kind = kind;
    now_string(slot->timestamp, sizeof(slot->timestamp));
    snprintf(slot->username, sizeof(slot->username), "%s",
             username != NULL ? username : "-");
    snprintf(slot->address, sizeof(slot->address), "%s",
             address != NULL ? address : "local");
    snprintf(slot->detail, sizeof(slot->detail), "%s",
             detail != NULL ? detail : "");

    g_authlog_next = (g_authlog_next + 1) % RC_AUTH_LOG_SIZE;
    if (g_authlog_count < RC_AUTH_LOG_SIZE)
    {
        g_authlog_count++;
    }

    /* Security-relevant entries also go to stderr so an unattended run leaves
     * a trace even when nobody is watching the log pane. */
    if (kind == AUTHLOG_LOGON_FAIL || kind == AUTHLOG_LOCKED || kind == AUTHLOG_PERMISSION_DENIED)
    {
        fprintf(stderr, "[AUTH] %s %-12s %-16s %s\n",
                slot->timestamp, rc_authlog_kind_name(kind), slot->username, slot->detail);
        fflush(stderr);
    }
}

int rc_authlog_count(void)
{
    return g_authlog_count;
}

bool rc_authlog_get_at(int index_from_newest, RcAuthLogEntry *out)
{
    int slot;

    if (out == NULL || index_from_newest < 0 || index_from_newest >= g_authlog_count)
    {
        return false;
    }
    slot = (g_authlog_next - 1 - index_from_newest + RC_AUTH_LOG_SIZE * 2) % RC_AUTH_LOG_SIZE;
    *out = g_authlog[slot];
    return true;
}

int rc_authlog_clear(void)
{
    int cleared = g_authlog_count;

    memset(g_authlog, 0, sizeof(g_authlog));
    g_authlog_count = 0;
    g_authlog_next = 0;
    return cleared;
}

void rc_authlog_format(int count, char *buffer, size_t size)
{
    size_t used = 0;
    int i;

    if (buffer == NULL || size == 0)
    {
        return;
    }
    buffer[0] = '\0';
    if (count <= 0 || count > g_authlog_count)
    {
        count = g_authlog_count;
    }

    for (i = 0; i < count; ++i)
    {
        RcAuthLogEntry entry;

        if (!rc_authlog_get_at(i, &entry))
        {
            continue;
        }
        used += (size_t)snprintf(buffer + used, size - used,
                                 "  %s  %-12s %-14s %s\n",
                                 entry.timestamp, rc_authlog_kind_name(entry.kind),
                                 entry.username, entry.detail);
        if (used >= size)
        {
            break;
        }
    }
}

/* ==========================================================================
 * Accounts
 * ========================================================================== */
static int account_slot_by_id(int account_id)
{
    return account_id - 1;
}

static bool username_taken(const char *username, int except_id)
{
    int i;

    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == except_id)
        {
            continue;
        }
        if (strcmp(g_accounts[i].username, username) == 0)
        {
            return true;
        }
    }
    return false;
}

static int create_account_internal(const char *username, const char *password,
                                   const char *full_name, RcRole role,
                                   RcAccountStatus status)
{
    RcAccount *account;

    if (g_account_count >= RC_MAX_ACCOUNTS)
    {
        return -1;
    }
    if (username == NULL || username[0] == '\0')
    {
        return -1;
    }
    if (username_taken(username, 0))
    {
        return -1;
    }

    account = &g_accounts[g_account_count];
    memset(account, 0, sizeof(*account));
    account->id = g_next_account_id++;
    snprintf(account->username, sizeof(account->username), "%s", username);
    snprintf(account->full_name, sizeof(account->full_name), "%s",
             full_name != NULL ? full_name : "");
    account->role = role;
    account->status = status;
    account->created_ms = now_ms();
    account->password_changed_ms = account->created_ms;

    g_account_count++;

    if (password != NULL && password[0] != '\0')
    {
        rc_account_hash_password(account, password);
    }
    return account->id;
}

void rc_account_init(void)
{
    memset(g_accounts, 0, sizeof(g_accounts));
    memset(g_passwords, 0, sizeof(g_passwords));
    memset(g_sessions, 0, sizeof(g_sessions));
    memset(g_authlog, 0, sizeof(g_authlog));
    g_account_count = 0;
    g_authlog_count = 0;
    g_authlog_next = 0;
    g_authlog_next_id = 1;
    g_next_account_id = 1;
    g_next_session_id = 1;
    g_current_session = 0;

    g_policy.min_length = 8;
    g_policy.require_upper = true;
    g_policy.require_lower = true;
    g_policy.require_digit = true;
    g_policy.require_symbol = false;
    g_policy.max_failed_logons = 5;
    g_policy.session_timeout_minutes = 30;
    g_policy.must_change_default = true;

    /* ----------------------------------------------------------------------
     * Installed default accounts.
     *
     * THESE ARE DEMONSTRATION CREDENTIALS. Every one must be changed before
     * the program is used for anything but training, and must_change_default
     * forces that at first logon.
     * -------------------------------------------------------------------- */
    (void)create_account_internal("admin", "Admin#2024", "System Administrator",
                                  ROLE_ADMIN, ACCOUNT_MUST_CHANGE);
    (void)create_account_internal("signaller1", "Signal#2024", "Duty Signaller",
                                  ROLE_SIGNALLER, ACCOUNT_MUST_CHANGE);
    (void)create_account_internal("signaller2", "Signal#2024", "Relief Signaller",
                                  ROLE_SIGNALLER, ACCOUNT_MUST_CHANGE);
    (void)create_account_internal("supervisor", "Super#2024", "Shift Supervisor",
                                  ROLE_SUPERVISOR, ACCOUNT_MUST_CHANGE);
    (void)create_account_internal("viewer", "View#2024", "Observer / Trainee",
                                  ROLE_VIEWER, ACCOUNT_MUST_CHANGE);

    rc_authlog_write(AUTHLOG_SYSTEM, "SYSTEM", "local",
                     RC_PRODUCT_NAME " account store initialised with 5 default accounts");
}

void rc_account_shutdown(void)
{
    memset(g_sessions, 0, sizeof(g_sessions));
    g_current_session = 0;
}

int rc_account_count(void)
{
    return g_account_count;
}

const RcAccount *rc_account_get_at(int index)
{
    if (index < 0 || index >= g_account_count)
    {
        return NULL;
    }
    return &g_accounts[index];
}

const RcAccount *rc_account_find(const char *username)
{
    int i;

    if (username == NULL)
    {
        return NULL;
    }
    for (i = 0; i < g_account_count; ++i)
    {
        if (strcmp(g_accounts[i].username, username) == 0)
        {
            return &g_accounts[i];
        }
    }
    return NULL;
}

const RcAccount *rc_account_by_id(int account_id)
{
    const int slot = account_slot_by_id(account_id);

    if (slot < 0 || slot >= g_account_count)
    {
        return NULL;
    }
    if (g_accounts[slot].id != account_id)
    {
        return NULL;
    }
    return &g_accounts[slot];
}

/* ==========================================================================
 * Sessions
 * ========================================================================== */
int rc_session_count(void)
{
    int i;
    int count = 0;

    for (i = 0; i < RC_MAX_SESSIONS; ++i)
    {
        if (g_sessions[i].in_use)
        {
            count++;
        }
    }
    return count;
}

bool rc_session_get_at(int index, RcSession *out)
{
    int i;
    int seen = 0;

    if (out == NULL)
    {
        return false;
    }
    for (i = 0; i < RC_MAX_SESSIONS; ++i)
    {
        if (!g_sessions[i].in_use)
        {
            continue;
        }
        if (seen == index)
        {
            *out = g_sessions[i];
            return true;
        }
        seen++;
    }
    return false;
}

static RcSession *session_by_id(int session_id)
{
    int i;

    for (i = 0; i < RC_MAX_SESSIONS; ++i)
    {
        if (g_sessions[i].in_use && g_sessions[i].id == session_id)
        {
            return &g_sessions[i];
        }
    }
    return NULL;
}

static void destroy_session(RcSession *session, RcAuthLogKind kind, const char *detail)
{
    if (session == NULL)
    {
        return;
    }
    rc_authlog_write(kind, session->username, session->terminal, detail);
    session->in_use = false;
    if (g_current_session == session->id)
    {
        g_current_session = 0;
    }
}

RcAuthResult rc_logon(const char *username, const char *password,
                      const char *terminal, const char *address,
                      int *session_id)
{
    const RcAccount *found;
    RcAccount *account;
    RcSession *slot = NULL;
    int i;

    if (session_id != NULL)
    {
        *session_id = 0;
    }
    if (username == NULL || password == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    found = rc_account_find(username);
    if (found == NULL)
    {
        rc_authlog_write(AUTHLOG_LOGON_FAIL, username, address,
                         "unknown userid");
        return AUTH_ERR_UNKNOWN_USER;
    }

    account = &g_accounts[account_slot_by_id(found->id)];

    if (account->status == ACCOUNT_DISABLED)
    {
        rc_authlog_write(AUTHLOG_LOGON_FAIL, username, address,
                         "account is disabled");
        return AUTH_ERR_DISABLED;
    }
    if (account->status == ACCOUNT_LOCKED)
    {
        rc_authlog_write(AUTHLOG_LOCKED, username, address,
                         "logon attempt on a locked account");
        return AUTH_ERR_LOCKED;
    }

    if (!rc_account_verify_password(account, password))
    {
        account->failed_logons++;
        if (account->failed_logons >= g_policy.max_failed_logons)
        {
            account->status = ACCOUNT_LOCKED;
            rc_authlog_write(AUTHLOG_LOCKED, username, address,
                             "account locked after repeated failures");
            return AUTH_ERR_LOCKED;
        }
        rc_authlog_write(AUTHLOG_LOGON_FAIL, username, address,
                         "incorrect password");
        return AUTH_ERR_BAD_PASSWORD;
    }

    /* Success. Find a free session slot. */
    for (i = 0; i < RC_MAX_SESSIONS; ++i)
    {
        if (!g_sessions[i].in_use)
        {
            slot = &g_sessions[i];
            break;
        }
    }
    if (slot == NULL)
    {
        rc_authlog_write(AUTHLOG_LOGON_FAIL, username, address,
                         "no free session slot");
        return AUTH_ERR_NO_SESSION;
    }

    memset(slot, 0, sizeof(*slot));
    slot->id = g_next_session_id++;
    slot->account_id = account->id;
    snprintf(slot->username, sizeof(slot->username), "%s", account->username);
    slot->role = account->role;
    snprintf(slot->terminal, sizeof(slot->terminal), "%s",
             terminal != NULL ? terminal : "GUI");
    slot->started_ms = now_ms();
    slot->last_seen_ms = slot->started_ms;
    slot->expires_ms = slot->started_ms + (unsigned long)g_policy.session_timeout_minutes * 60000UL;
    slot->in_use = true;

    account->failed_logons = 0;
    account->last_logon_ms = slot->started_ms;
    account->logon_count++;
    snprintf(account->last_address, sizeof(account->last_address), "%s",
             address != NULL ? address : "local");

    if (session_id != NULL)
    {
        *session_id = slot->id;
    }

    rc_authlog_write(AUTHLOG_LOGON_OK, account->username, slot->terminal,
                     account->status == ACCOUNT_MUST_CHANGE
                         ? "logon accepted - password change required"
                         : "logon accepted");

    if (account->status == ACCOUNT_MUST_CHANGE)
    {
        /* The session is valid but the operator is reminded every logon until
         * the password is changed. */
    }
    return AUTH_OK;
}

RcAuthResult rc_logoff(int session_id)
{
    RcSession *session = session_by_id(session_id);

    if (session == NULL)
    {
        return AUTH_ERR_EXPIRED;
    }
    destroy_session(session, AUTHLOG_LOGOFF, "logoff requested");
    return AUTH_OK;
}

RcAuthResult rc_session_check(int session_id, RcPermission permission)
{
    RcSession *session = session_by_id(session_id);
    const RcAccount *account;

    if (session == NULL)
    {
        return AUTH_ERR_EXPIRED;
    }
    if (now_ms() >= session->expires_ms)
    {
        destroy_session(session, AUTHLOG_TIMEOUT, "session timed out");
        return AUTH_ERR_EXPIRED;
    }
    if (permission >= 0)
    {
        account = rc_account_by_id(session->account_id);
        if (!rc_account_can(account, permission))
        {
            rc_authlog_write(AUTHLOG_PERMISSION_DENIED, session->username,
                             session->terminal,
                             rc_permission_name(permission));
            return AUTH_ERR_NOT_PERMITTED;
        }
    }
    return AUTH_OK;
}

RcAuthResult rc_session_touch(int session_id)
{
    RcSession *session = session_by_id(session_id);

    if (session == NULL)
    {
        return AUTH_ERR_EXPIRED;
    }
    session->last_seen_ms = now_ms();
    session->expires_ms = session->last_seen_ms + (unsigned long)g_policy.session_timeout_minutes * 60000UL;
    session->actions_performed++;
    return AUTH_OK;
}

bool rc_session_can(int session_id, RcPermission permission)
{
    return rc_session_check(session_id, permission) == AUTH_OK;
}

const RcAccount *rc_session_account(int session_id)
{
    const RcSession *session = session_by_id(session_id);

    if (session == NULL)
    {
        return NULL;
    }
    return rc_account_by_id(session->account_id);
}

RcRole rc_session_role(int session_id)
{
    const RcSession *session = session_by_id(session_id);

    return (session != NULL) ? session->role : ROLE_VIEWER;
}

/* ==========================================================================
 * Current operator convenience layer
 * ========================================================================== */
void rc_set_current_session(int session_id)
{
    g_current_session = session_id;
}

int rc_current_session(void)
{
    return g_current_session;
}

bool rc_can(RcPermission permission)
{
    if (g_current_session == 0)
    {
        return false;
    }
    return rc_session_check(g_current_session, permission) == AUTH_OK;
}

bool rc_require(RcPermission permission)
{
    RcAuthResult result;
    const RcAccount *account;

    if (g_current_session == 0)
    {
        rc_authlog_write(AUTHLOG_PERMISSION_DENIED, "-", "local",
                         "action attempted with no session");
        return false;
    }

    result = rc_session_check(g_current_session, permission);
    if (result == AUTH_OK)
    {
        return true;
    }

    account = rc_session_account(g_current_session);
    rc_authlog_write(AUTHLOG_PERMISSION_DENIED,
                     account != NULL ? account->username : "-",
                     "local",
                     rc_permission_name(permission));
    return false;
}

/* ==========================================================================
 * Account administration
 * ========================================================================== */
RcAuthResult rc_account_create(int session_id, const char *username,
                               const char *password, const char *full_name,
                               RcRole role, int *new_account_id)
{
    const char *problem;
    int new_id;
    char detail[160];
    const RcAccount *actor;

    if (new_account_id != NULL) {
        *new_account_id = -1;
    }
    if (!rc_session_can(session_id, PERM_MANAGE_ACCOUNTS)) {
        return AUTH_ERR_NOT_PERMITTED;
    }
    if (username == NULL || username[0] == '\0') {
        return AUTH_ERR_UNKNOWN_USER;
    }
    if (username_taken(username, 0)) {
        return AUTH_ERR_UNKNOWN_USER;
    }

    problem = rc_password_validate(password);
    if (problem != NULL) {
        rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, username, "local", problem);
        return AUTH_ERR_BAD_PASSWORD;
    }

    new_id = create_account_internal(username, password, full_name, role, ACCOUNT_ACTIVE);
    if (new_id < 0) {
        return AUTH_ERR_UNKNOWN_USER;
    }
    if (new_account_id != NULL) {
        *new_account_id = new_id;
    }

    actor = rc_session_account(session_id);
    snprintf(detail, sizeof(detail), "created by %s with role %s",
             actor != NULL ? actor->username : "-", rc_role_name(role));
    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, username, "local", detail);
    return AUTH_OK;
}

RcAuthResult rc_account_remove(int session_id, int account_id)
{
    const RcAccount *actor;
    RcAccount *account;
    int i;
    int admins = 0;
    char detail[160];

    if (!rc_session_can(session_id, PERM_MANAGE_ACCOUNTS))
    {
        return AUTH_ERR_NOT_PERMITTED;
    }

    account = NULL;
    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == account_id)
        {
            account = &g_accounts[i];
            break;
        }
    }
    if (account == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    actor = rc_session_account(session_id);

    /* Never let an administrator delete themselves, and never leave the
     * installation without an administrator. */
    if (actor != NULL && actor->id == account_id)
    {
        rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local",
                         "refused: cannot remove the signed-in account");
        return AUTH_ERR_NOT_PERMITTED;
    }
    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].role == ROLE_ADMIN && g_accounts[i].status != ACCOUNT_DISABLED)
        {
            admins++;
        }
    }
    if (account->role == ROLE_ADMIN && admins <= 1)
    {
        rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local",
                         "refused: would leave no administrator");
        return AUTH_ERR_NOT_PERMITTED;
    }

    snprintf(detail, sizeof(detail), "removed by %s",
             actor != NULL ? actor->username : "-");
    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local", detail);

    /* Destroy any live sessions, then compact the array. */
    for (i = 0; i < RC_MAX_SESSIONS; ++i)
    {
        if (g_sessions[i].in_use && g_sessions[i].account_id == account_id)
        {
            g_sessions[i].in_use = false;
        }
    }

    {
        const int slot = account_slot_by_id(account_id);
        int j;
        for (j = slot; j + 1 < g_account_count; ++j)
        {
            g_accounts[j] = g_accounts[j + 1];
            g_passwords[j] = g_passwords[j + 1];
        }
        g_account_count--;
        memset(&g_accounts[g_account_count], 0, sizeof(g_accounts[0]));
        memset(&g_passwords[g_account_count], 0, sizeof(g_passwords[0]));
    }
    return AUTH_OK;
}

RcAuthResult rc_account_set_role(int session_id, int account_id, RcRole role)
{
    RcAccount *account = NULL;
    const RcAccount *actor;
    int i;
    int admins = 0;
    char detail[160];

    if (!rc_session_can(session_id, PERM_MANAGE_ACCOUNTS))
    {
        return AUTH_ERR_NOT_PERMITTED;
    }
    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == account_id)
        {
            account = &g_accounts[i];
            break;
        }
    }
    if (account == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    /* Refuse to remove the last administrator. */
    if (account->role == ROLE_ADMIN && role != ROLE_ADMIN)
    {
        for (i = 0; i < g_account_count; ++i)
        {
            if (g_accounts[i].role == ROLE_ADMIN && g_accounts[i].status != ACCOUNT_DISABLED)
            {
                admins++;
            }
        }
        if (admins <= 1)
        {
            return AUTH_ERR_NOT_PERMITTED;
        }
    }

    actor = rc_session_account(session_id);
    snprintf(detail, sizeof(detail), "role %s -> %s by %s",
             rc_role_name(account->role), rc_role_name(role),
             actor != NULL ? actor->username : "-");

    account->role = role;
    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local", detail);

    /* Re-stamp every live session so the role change takes effect at once. */
    for (i = 0; i < RC_MAX_SESSIONS; ++i)
    {
        if (g_sessions[i].in_use && g_sessions[i].account_id == account_id)
        {
            g_sessions[i].role = role;
        }
    }
    return AUTH_OK;
}

RcAuthResult rc_account_set_status(int session_id, int account_id, RcAccountStatus status)
{
    RcAccount *account = NULL;
    const RcAccount *actor;
    int i;
    char detail[160];

    if (!rc_session_can(session_id, PERM_MANAGE_ACCOUNTS))
    {
        return AUTH_ERR_NOT_PERMITTED;
    }
    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == account_id)
        {
            account = &g_accounts[i];
            break;
        }
    }
    if (account == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    actor = rc_session_account(session_id);
    if (actor != NULL && actor->id == account_id && status == ACCOUNT_DISABLED)
    {
        rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local",
                         "refused: cannot disable the signed-in account");
        return AUTH_ERR_NOT_PERMITTED;
    }

    snprintf(detail, sizeof(detail), "status %s -> %s by %s",
             rc_account_status_name(account->status), rc_account_status_name(status),
             actor != NULL ? actor->username : "-");

    account->status = status;
    if (status == ACCOUNT_ACTIVE)
    {
        account->failed_logons = 0;
    }

    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local", detail);

    /* Disabling an account must end its sessions immediately. */
    if (status == ACCOUNT_DISABLED || status == ACCOUNT_LOCKED)
    {
        for (i = 0; i < RC_MAX_SESSIONS; ++i)
        {
            if (g_sessions[i].in_use && g_sessions[i].account_id == account_id)
            {
                g_sessions[i].in_use = false;
            }
        }
    }
    return AUTH_OK;
}

RcAuthResult rc_account_unlock(int session_id, int account_id)
{
    return rc_account_set_status(session_id, account_id, ACCOUNT_ACTIVE);
}

RcAuthResult rc_account_change_password(int session_id, int account_id,
                                        const char *old_password,
                                        const char *new_password)
{
    RcAccount *account = NULL;
    const RcAccount *actor;
    const char *problem;
    bool is_self;
    bool is_admin;
    int i;

    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == account_id)
        {
            account = &g_accounts[i];
            break;
        }
    }
    if (account == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    actor = rc_session_account(session_id);
    is_self = (actor != NULL && actor->id == account_id);
    is_admin = rc_session_can(session_id, PERM_MANAGE_ACCOUNTS);

    if (!is_self && !is_admin)
    {
        rc_authlog_write(AUTHLOG_PERMISSION_DENIED, account->username, "local",
                         "password change for another account");
        return AUTH_ERR_NOT_PERMITTED;
    }

    /* A user changing their own password must prove the old one. An
     * administrator may reset without it, which is the usual escape hatch
     * when a user is locked out. */
    if (is_self && !is_admin)
    {
        if (!rc_account_verify_password(account, old_password))
        {
            rc_authlog_write(AUTHLOG_LOGON_FAIL, account->username, "local",
                             "password change with an incorrect current password");
            return AUTH_ERR_BAD_PASSWORD;
        }
    }

    problem = rc_password_validate(new_password);
    if (problem != NULL)
    {
        rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local", problem);
        return AUTH_ERR_BAD_PASSWORD;
    }

    if (old_password != NULL && new_password != NULL && strcmp(old_password, new_password) == 0)
    {
        rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local",
                         "new password is identical to the old one");
        return AUTH_ERR_BAD_PASSWORD;
    }

    rc_account_hash_password(account, new_password);
    account->status = ACCOUNT_ACTIVE;
    account->failed_logons = 0;

    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local",
                     is_self ? "password changed by the user"
                             : "password reset by an administrator");
    return AUTH_OK;
}

RcAuthResult rc_account_grant(int session_id, int account_id, RcPermission permission)
{
    RcAccount *account = NULL;
    int i;
    char detail[160];

    if (!rc_session_can(session_id, PERM_MANAGE_ACCOUNTS))
    {
        return AUTH_ERR_NOT_PERMITTED;
    }
    if (permission < 0 || permission >= PERM_COUNT)
    {
        return AUTH_ERR_NOT_PERMITTED;
    }
    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == account_id)
        {
            account = &g_accounts[i];
            break;
        }
    }
    if (account == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    account->granted_extra |= (1UL << (int)permission);
    account->revoked &= ~(1UL << (int)permission);

    snprintf(detail, sizeof(detail), "granted %s", rc_permission_name(permission));
    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local", detail);
    return AUTH_OK;
}

RcAuthResult rc_account_revoke(int session_id, int account_id, RcPermission permission)
{
    RcAccount *account = NULL;
    int i;
    char detail[160];

    if (!rc_session_can(session_id, PERM_MANAGE_ACCOUNTS))
    {
        return AUTH_ERR_NOT_PERMITTED;
    }
    if (permission < 0 || permission >= PERM_COUNT)
    {
        return AUTH_ERR_NOT_PERMITTED;
    }
    for (i = 0; i < g_account_count; ++i)
    {
        if (g_accounts[i].id == account_id)
        {
            account = &g_accounts[i];
            break;
        }
    }
    if (account == NULL)
    {
        return AUTH_ERR_UNKNOWN_USER;
    }

    account->revoked |= (1UL << (int)permission);
    account->granted_extra &= ~(1UL << (int)permission);

    snprintf(detail, sizeof(detail), "revoked %s", rc_permission_name(permission));
    rc_authlog_write(AUTHLOG_ACCOUNT_CHANGE, account->username, "local", detail);
    return AUTH_OK;
}

/* ==========================================================================
 * Result text
 * ========================================================================== */
const char *rc_auth_result_name(RcAuthResult result)
{
    switch (result)
    {
    case AUTH_OK:
        return "OK";
    case AUTH_ERR_UNKNOWN_USER:
        return "UNKNOWN-USER";
    case AUTH_ERR_BAD_PASSWORD:
        return "BAD-PASSWORD";
    case AUTH_ERR_LOCKED:
        return "ACCOUNT-LOCKED";
    case AUTH_ERR_DISABLED:
        return "ACCOUNT-DISABLED";
    case AUTH_ERR_NO_SESSION:
        return "NO-SESSION-SLOT";
    case AUTH_ERR_EXPIRED:
        return "SESSION-EXPIRED";
    case AUTH_ERR_NOT_PERMITTED:
        return "NOT-PERMITTED";
    }
    return "AUTH-ERROR";
}

const char *rc_auth_result_message(RcAuthResult result)
{
    switch (result)
    {
    case AUTH_OK:
        return "Accepted.";
    case AUTH_ERR_UNKNOWN_USER:
        return "Userid not recognised.";
    case AUTH_ERR_BAD_PASSWORD:
        return "Password incorrect.";
    case AUTH_ERR_LOCKED:
        return "Account locked after repeated failures.";
    case AUTH_ERR_DISABLED:
        return "Account withdrawn by an administrator.";
    case AUTH_ERR_NO_SESSION:
        return "Too many concurrent sessions.";
    case AUTH_ERR_EXPIRED:
        return "Session expired - please log on again.";
    case AUTH_ERR_NOT_PERMITTED:
        return "Your role does not hold that permission.";
    }
    return "Authentication failed.";
}

/* ==========================================================================
 * Persistence
 * ========================================================================== */
int rc_account_save(const char *path)
{
    FILE *file;
    int i;

    if (path == NULL)
    {
        return -1;
    }
    file = fopen(path, "w");
    if (file == NULL)
    {
        return -1;
    }

    fprintf(file, "# %s account store\n", RC_PRODUCT_NAME);
    fprintf(file, "# Generated by %s %s\n", RC_DEVELOPER_NAME, RC_VERSION_SHORT);
    fprintf(file, "# Contains password hashes. Protect this file.\n\n");

    for (i = 0; i < g_account_count; ++i)
    {
        const RcAccount *account = &g_accounts[i];
        fprintf(file, "[account]\n");
        fprintf(file, "id = %d\n", account->id);
        fprintf(file, "username = %s\n", account->username);
        fprintf(file, "full_name = %s\n", account->full_name);
        fprintf(file, "role = %s\n", rc_role_name(account->role));
        fprintf(file, "status = %s\n", rc_account_status_name(account->status));
        fprintf(file, "failed_logons = %d\n", account->failed_logons);
        fprintf(file, "granted_extra = %lu\n", account->granted_extra);
        fprintf(file, "revoked = %lu\n", account->revoked);
        fprintf(file, "salt = %s\n", g_passwords[i].salt);
        fprintf(file, "hash = %s\n\n", g_passwords[i].hash);
    }

    if (fclose(file) != 0)
    {
        return -1;
    }
    return 0;
}

int rc_account_export_redacted(const char *path)
{
    FILE *file;
    int i;

    if (path == NULL)
    {
        return -1;
    }
    file = fopen(path, "w");
    if (file == NULL)
    {
        return -1;
    }

    fprintf(file, "# %s account export (REDACTED - no hashes)\n", RC_PRODUCT_NAME);
    fprintf(file, "# Safe to share for review. Generated by %s.\n\n",
            RC_DEVELOPER_NAME);
    fprintf(file, "%-4s %-14s %-10s %-12s %-8s %s\n",
            "ID", "USERNAME", "ROLE", "STATUS", "FAILS", "PERMISSIONS");
    fprintf(file, "---- -------------- ---------- ------------ ------- "
                  "----------------------------------------\n");

    for (i = 0; i < g_account_count; ++i)
    {
        const RcAccount *account = &g_accounts[i];
        fprintf(file, "%-4d %-14s %-10s %-12s %-8d %lu\n",
                account->id, account->username, rc_role_name(account->role),
                rc_account_status_name(account->status), account->failed_logons,
                rc_account_permissions(account));
    }

    fclose(file);
    return 0;
}

int rc_account_load(const char *path)
{
    FILE *file;
    char line[512];

    if (path == NULL)
    {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL)
    {
        return -1;
    }

    /* A full loader is deliberately not implemented: importing an account
     * file silently would be a privilege escalation path. The redacted export
     * is provided so accounts can be reviewed, and rc_account_create() is the
     * only way in. */
    while (fgets(line, (int)sizeof(line), file) != NULL)
    {
        if (line[0] != '#' && line[0] != '\n' && line[0] != '[')
        {
            rc_authlog_write(AUTHLOG_SYSTEM, "SYSTEM", "local",
                             "account import ignored - use rc_account_create()");
            break;
        }
    }

    fclose(file);
    return 0;
}
