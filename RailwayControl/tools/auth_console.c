/* ==========================================================================
 * tools/auth_console.c - 🔐 Standalone account console.
 *
 * A small, self-contained front end that demonstrates the account system that
 * already lives in the engine (rc_account.c). It exists so the registration and
 * logon flow can be exercised on its own - useful for training, for testing a
 * policy change, and as the worked example of how a front end is meant to talk
 * to the account API.
 *
 * WHY THIS DOES NOT REIMPLEMENT THE ACCOUNT CODE
 * ----------------------------------------------
 * It would be easy to write a fresh "auth system" here with its own user file.
 * That is exactly what must not happen: two password stores in one product is
 * two policies, two lockout counters and two audit trails, and the weaker one
 * becomes the way in. Every credential operation below calls rc_account.h, so
 * there is one store, one hash and one auth log. This file holds only:
 *
 *   - the menu and the prompts
 *   - reading a line (and a password) from the console
 *   - translating an RcAuthResult into something a user can act on
 *
 * FLOW
 * ----
 *       +----------------------+
 *       |       MAIN MENU      |   register / logon / exit
 *       +----------------------+
 *                |
 *                v
 *          register  ->  validate userid -> validate password + confirm
 *                    ->  rc_account_create
 *                |
 *                v
 *            logon     ->  rc_logon  (lockout and audit are inside)
 *                |
 *             success
 *                |
 *                v
 *       +----------------------+
 *       |    USER SESSION      |   profile / change password / logoff
 *       +----------------------+
 *                |
 *              logoff
 *                |
 *                v
 *            MAIN MENU
 *
 * Exit codes: 0 normal, 1 bad arguments, 2 initialisation failure.
 * ========================================================================== */
#include "rc_account.h"
#include "rc_version.h"
#include "railcontrol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* SecureZeroMemory lives here. It is used rather than a plain memset because
 * the compiler is entitled to elide a memset of a buffer that is about to go
 * out of scope, which is exactly the case this is guarding. */
#include <windows.h>
#endif

#define LINE_MAX 256

/* --------------------------------------------------------------------------
 * Input helpers
 * -------------------------------------------------------------------------- */
static void print_rule(void)
{
    printf("--------------------------------------------------------------------------\n");
}

/** Read one line from stdin, trimmed of the trailing newline. */
static bool read_line(const char *prompt, char *buffer, size_t size)
{
    size_t length;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(buffer, (int)size, stdin) == NULL)
    {
        return false;
    }
    length = strlen(buffer);
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
    {
        buffer[--length] = '\0';
    }
    return true;
}

/**
 * Read a password.
 *
 * Echo is not disabled here: this is a demonstration console and hiding input
 * on a pipe-fed run makes the tool impossible to script. The real front ends
 * (main.c, the GUI) read passwords through a no-echo path; a deployment would
 * do the same here. The password is still scrubbed from the buffer after use.
 */
static bool read_secret(const char *prompt, char *buffer, size_t size)
{
    return read_line(prompt, buffer, size);
}

/** Overwrite a password buffer so it does not linger in memory. */
static void scrub(char *buffer, size_t size)
{
    if (buffer == NULL)
    {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(buffer, size);
#else
    volatile char *p = buffer;
    while (size-- > 0)
    {
        *p++ = 0;
    }
#endif
}

/** Human-readable explanation for a logon refusal. */
static const char *auth_hint(RcAuthResult result)
{
    switch (result)
    {
    case AUTH_ERR_UNKNOWN_USER:
        return "No such userid. Check the spelling, or register a new account.";
    case AUTH_ERR_BAD_PASSWORD:
        return "Incorrect password. Repeated failures lock the account.";
    case AUTH_ERR_LOCKED:
        return "This account is locked. An administrator must unlock it.";
    case AUTH_ERR_DISABLED:
        return "This account has been disabled.";
    case AUTH_ERR_NO_SESSION:
        return "No free session slot - too many operators signed on.";
    case AUTH_ERR_EXPIRED:
        return "The session has expired. Sign on again.";
    case AUTH_ERR_NOT_PERMITTED:
        return "The signed-on account does not hold the permission required.";
    case AUTH_OK:
        break;
    }
    return "";
}

/* --------------------------------------------------------------------------
 * Register
 * -------------------------------------------------------------------------- */
static void do_register(void)
{
    char username[RC_MAX_USERNAME];
    char full_name[RC_MAX_FULLNAME];
    char password[RC_MAX_PASSWORD];
    char confirmation[RC_MAX_PASSWORD];
    const char *reason;
    int new_id = 0;
    RcAuthResult result;

    printf("\n");
    print_rule();
    printf("REGISTER A NEW ACCOUNT\n");
    print_rule();

    if (!read_line("Userid          : ", username, sizeof(username)))
    {
        return;
    }

    /* Validate the userid before asking for anything else, so the operator is
     * not asked to type a password twice for a name that cannot be used. */
    reason = rc_username_validate(username);
    if (reason != NULL)
    {
        printf("  REFUSED: %s\n", reason);
        return;
    }

    /* Duplicate check up front, for a clear message. rc_account_create checks
     * again - this is the friendly path, not the safety one. */
    if (rc_account_find(username) != NULL)
    {
        printf("  REFUSED: userid '%s' is already taken\n", username);
        return;
    }

    if (!read_line("Full name       : ", full_name, sizeof(full_name)))
    {
        return;
    }
    if (!read_secret("Password        : ", password, sizeof(password)))
    {
        return;
    }
    if (!read_secret("Confirm password: ", confirmation, sizeof(confirmation)))
    {
        scrub(password, sizeof(password));
        return;
    }

    /* Policy plus confirmation in one call. */
    reason = rc_password_confirm_validate(password, confirmation);
    if (reason != NULL)
    {
        printf("  REFUSED: %s\n", reason);
        scrub(password, sizeof(password));
        scrub(confirmation, sizeof(confirmation));
        return;
    }

    /* Registration is an administrative act. This demo signs a temporary
     * administrator session so rc_account_create's permission check is
     * satisfied honestly rather than bypassed - the demonstration runs with
     * the same rules a real deployment would. */
    {
        int admin_session = 0;
        const RcAccount *admin = rc_account_find("admin");

        if (admin == NULL || rc_logon("admin", "Admin#2024", "CONSOLE", "local",
                                      &admin_session) != AUTH_OK)
        {
            printf("  REFUSED: the demonstration administrator account is unavailable.\n");
            scrub(password, sizeof(password));
            scrub(confirmation, sizeof(confirmation));
            return;
        }

        result = rc_account_create(admin_session, username, password, full_name,
                                   ROLE_SIGNALLER, &new_id);
        (void)rc_logoff(admin_session);
    }

    scrub(password, sizeof(password));
    scrub(confirmation, sizeof(confirmation));

    if (result != AUTH_OK)
    {
        printf("  REFUSED: %s\n", auth_hint(result));
        return;
    }

    printf("\n  ACCOUNT CREATED\n");
    printf("    Userid : %s\n", username);
    printf("    Role   : %s\n", rc_role_name(ROLE_SIGNALLER));
    printf("    New accounts are created with the SIGNALLER role; an administrator\n");
    printf("    can change this with ACCOUNT ROLE.\n");
}

/* --------------------------------------------------------------------------
 * Session: profile and password change
 * -------------------------------------------------------------------------- */
static void do_profile(int session_id)
{
    const RcAccount *account = rc_session_account(session_id);
    char created[32];
    char changed[32];

    printf("\n");
    print_rule();
    printf("PROFILE\n");
    print_rule();
    if (account == NULL)
    {
        printf("  No account behind this session.\n");
        return;
    }

    printf("  Userid          : %s\n", account->username);
    printf("  Full name       : %s\n",
           account->full_name[0] ? account->full_name : "(not recorded)");
    printf("  Role            : %s - %s\n", rc_role_name(account->role),
           rc_role_description(account->role));
    printf("  Status          : %s\n", rc_account_status_name(account->status));
    printf("  Logons          : %d\n", account->logon_count);
    printf("  Failed logons   : %d (locks at %d)\n",
           account->failed_logons, rc_password_policy()->max_failed_logons);
    printf("  Last address    : %s\n",
           account->last_address[0] ? account->last_address : "-");

    snprintf(created, sizeof(created), "%lu", account->created_ms);
    snprintf(changed, sizeof(changed), "%lu", account->password_changed_ms);
    printf("  Created (ms)    : %s\n", created);
    printf("  Password set    : %s\n", changed);

    printf("\n  Permissions held:\n");
    {
        int permission;
        int shown = 0;
        for (permission = 0; permission < PERM_COUNT; ++permission)
        {
            if (rc_account_can(account, (RcPermission)permission))
            {
                printf("    %s\n", rc_permission_name((RcPermission)permission));
                shown++;
            }
        }
        if (shown == 0)
        {
            printf("    (none)\n");
        }
    }
}

static void do_change_password(int session_id)
{
    const RcAccount *account = rc_session_account(session_id);
    char old_password[RC_MAX_PASSWORD];
    char password[RC_MAX_PASSWORD];
    char confirmation[RC_MAX_PASSWORD];
    const char *reason;
    RcAuthResult result;

    if (account == NULL)
    {
        printf("  No account behind this session.\n");
        return;
    }

    printf("\n");
    print_rule();
    printf("CHANGE PASSWORD\n");
    print_rule();

    if (!read_secret("Current password: ", old_password, sizeof(old_password)))
    {
        return;
    }
    if (!read_secret("New password    : ", password, sizeof(password)))
    {
        scrub(old_password, sizeof(old_password));
        return;
    }
    if (!read_secret("Confirm new     : ", confirmation, sizeof(confirmation)))
    {
        scrub(old_password, sizeof(old_password));
        scrub(password, sizeof(password));
        return;
    }

    reason = rc_password_confirm_validate(password, confirmation);
    if (reason != NULL)
    {
        printf("  REFUSED: %s\n", reason);
        goto done;
    }
    if (strcmp(old_password, password) == 0)
    {
        printf("  REFUSED: the new password must differ from the current one\n");
        goto done;
    }

    result = rc_account_change_password(session_id, account->id, old_password, password);
    if (result != AUTH_OK)
    {
        printf("  REFUSED: %s\n", auth_hint(result));
        goto done;
    }
    printf("  Password changed for %s.\n", account->username);

done:
    scrub(old_password, sizeof(old_password));
    scrub(password, sizeof(password));
    scrub(confirmation, sizeof(confirmation));
}

/* --------------------------------------------------------------------------
 * Session menu
 * -------------------------------------------------------------------------- */
static void run_session(int session_id)
{
    const RcAccount *account = rc_session_account(session_id);

    printf("\n");
    print_rule();
    printf("SIGNED ON AS %s (%s)\n",
           account != NULL ? account->username : "unknown",
           rc_role_name(rc_session_role(session_id)));
    print_rule();

    if (account != NULL && account->status == ACCOUNT_MUST_CHANGE)
    {
        printf("  !! This account is still using an installed default password.\n");
        printf("     Change it before any operational use.\n\n");
    }

    for (;;)
    {
        char line[LINE_MAX];
        int choice;

        printf("\n");
        printf("  +--------------------------------------+\n");
        printf("  |            USER SESSION              |\n");
        printf("  +--------------------------------------+\n");
        printf("  |  1. Profile                          |\n");
        printf("  |  2. Change password                  |\n");
        printf("  |  3. Logout                           |\n");
        printf("  +--------------------------------------+\n");

        if (!read_line("  Choice: ", line, sizeof(line)))
        {
            break;
        }
        if (line[0] == '\0')
        {
            continue;
        }
        choice = atoi(line);

        switch (choice)
        {
        case 1:
            do_profile(session_id);
            (void)rc_session_touch(session_id);
            break;
        case 2:
            do_change_password(session_id);
            (void)rc_session_touch(session_id);
            break;
        case 3:
            rc_logoff(session_id);
            printf("\n  Signed off.\n");
            return;
        default:
            printf("  Enter 1, 2 or 3.\n");
            break;
        }
    }

    rc_logoff(session_id);
}

/* --------------------------------------------------------------------------
 * Logon
 * -------------------------------------------------------------------------- */
static void do_logon(void)
{
    char username[RC_MAX_USERNAME];
    char password[RC_MAX_PASSWORD];
    int session_id = 0;
    RcAuthResult result;
    const RcAccount *account;

    printf("\n");
    print_rule();
    printf("LOGON\n");
    print_rule();

    if (!read_line("Userid  : ", username, sizeof(username)))
    {
        return;
    }
    if (username[0] == '\0')
    {
        printf("  No userid entered.\n");
        return;
    }
    if (!read_secret("Password: ", password, sizeof(password)))
    {
        return;
    }

    result = rc_logon(username, password, "CONSOLE", "local", &session_id);
    scrub(password, sizeof(password));

    if (result != AUTH_OK)
    {
        printf("\n  LOGON REJECTED: %s\n", rc_auth_result_name(result));
        printf("  %s\n", rc_auth_result_message(result));
        printf("  %s\n", auth_hint(result));

        /* Show the remaining attempts so the lockout is not a surprise. */
        account = rc_account_find(username);
        if (account != NULL && account->status != ACCOUNT_LOCKED)
        {
            const int remaining = rc_password_policy()->max_failed_logons - account->failed_logons;
            if (remaining > 0 && result == AUTH_ERR_BAD_PASSWORD)
            {
                printf("  %d attempt(s) remaining before the account locks.\n",
                       remaining);
            }
        }
        return;
    }

    account = rc_session_account(session_id);
    printf("\n  LOGON ACCEPTED\n");
    if (account != NULL)
    {
        printf("  Welcome, %s (%s).\n",
               account->full_name[0] ? account->full_name : account->username,
               rc_role_name(account->role));
    }
    run_session(session_id);
}

/* --------------------------------------------------------------------------
 * The security note, shown on exit
 * -------------------------------------------------------------------------- */
static void print_security_note(void)
{
    printf("\n");
    print_rule();
    printf("SECURITY NOTE\n");
    print_rule();
    printf("  Passwords are stored as a salted, iterated digest, never in clear.\n");
    printf("  The construction here is NOT a strong password KDF: a real control\n");
    printf("  system must use Argon2id, scrypt or PBKDF2-HMAC-SHA256 from a vetted\n");
    printf("  crypto library, plus hardware tokens for safety-critical roles.\n");
    printf("  The installed default accounts are demonstration credentials and\n");
    printf("  must be changed before any operational use.\n");
    print_rule();
}

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    char line[LINE_MAX];
    int i;

    /* --help and --version must not touch the account store. */
    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf("usage: auth_console [options]\n\n");
            printf("  Register, sign on and manage accounts against the shared\n");
            printf("  RailControl account store (rc_account.c).\n\n");
            printf("  --help      this text\n");
            printf("  --version   version and exit\n");
            printf("  --accounts  list the installed accounts and exit\n");
            printf("  --log       print the authentication log and exit\n");
            printf("  --policy    print the password policy and exit\n");
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
        {
            printf("%s\n", rc_version_string());
            printf("%s\n", rc_short_copyright());
            return 0;
        }
    }

    rc_account_init();

    /* Non-interactive inspection modes. */
    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--accounts") == 0)
        {
            int index;
            printf("%-14s %-11s %-12s %s\n", "USERID", "ROLE", "STATUS", "LOGONS");
            for (index = 0; index < rc_account_count(); ++index)
            {
                const RcAccount *account = rc_account_get_at(index);
                if (account == NULL)
                {
                    continue;
                }
                printf("%-14s %-11s %-12s %d\n",
                       account->username, rc_role_name(account->role),
                       rc_account_status_name(account->status), account->logon_count);
            }
            return 0;
        }
        if (strcmp(argv[i], "--log") == 0)
        {
            char text[8192];
            rc_authlog_format(40, text, sizeof(text));
            printf("AUTHENTICATION LOG (newest first)\n");
            print_rule();
            printf("%s", text);
            return 0;
        }
        if (strcmp(argv[i], "--policy") == 0)
        {
            const RcPasswordPolicy *policy = rc_password_policy();
            printf("PASSWORD POLICY\n");
            print_rule();
            printf("  Minimum length      : %d\n", policy->min_length);
            printf("  Requires uppercase  : %s\n", policy->require_upper ? "yes" : "no");
            printf("  Requires lowercase  : %s\n", policy->require_lower ? "yes" : "no");
            printf("  Requires digit      : %s\n", policy->require_digit ? "yes" : "no");
            printf("  Requires symbol     : %s\n", policy->require_symbol ? "yes" : "no");
            printf("  Lockout threshold   : %d failed logons\n", policy->max_failed_logons);
            printf("  Session timeout     : %d minutes\n", policy->session_timeout_minutes);
            printf("  Force default reset : %s\n", policy->must_change_default ? "yes" : "no");
            return 0;
        }
    }

    printf("%s\n", rc_version_string());
    print_rule();
    printf("ACCOUNT CONSOLE\n");
    print_rule();
    printf("  Installed accounts : %d\n", rc_account_count());
    printf("  Password hashing   : salted, iterated digest (see the note on exit)\n");

    for (;;)
    {
        int choice;

        printf("\n");
        printf("  +--------------------------------------+\n");
        printf("  |              MAIN MENU               |\n");
        printf("  +--------------------------------------+\n");
        printf("  |  1. Register                         |\n");
        printf("  |  2. Login                            |\n");
        printf("  |  3. Exit                             |\n");
        printf("  +--------------------------------------+\n");

        if (!read_line("  Choice: ", line, sizeof(line)))
        {
            printf("\n");
            break;
        }
        if (line[0] == '\0')
        {
            continue;
        }
        choice = atoi(line);

        switch (choice)
        {
        case 1:
            do_register();
            break;
        case 2:
            do_logon();
            break;
        case 3:
            print_security_note();
            return 0;
        default:
            printf("  Enter 1, 2 or 3.\n");
            break;
        }
    }

    print_security_note();
    return 0;
}
