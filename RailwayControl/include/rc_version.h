/* ==========================================================================
 * rc_version.h - Version, build and authorship information.
 *
 * The title screen, the About dialogue and every log file header render from
 * these definitions so there is exactly one place to change them.
 * ========================================================================== */
#ifndef RC_VERSION_H
#define RC_VERSION_H

/* --------------------------------------------------------------------------
 * Product
 * -------------------------------------------------------------------------- */
#define RC_PRODUCT_NAME      "RailControl"
#define RC_PRODUCT_TITLE     "RailControl - Railway Traffic Control & Interlocking System"
#define RC_PRODUCT_SUBTITLE  "Railway Traffic Management & Interlocking Simulator"

/* Specification identity, matching railcontrol.h. */
#define RC_SPEC_NAME         "Railway Traffic Control & Interlocking System"
#define RC_SPEC_ABBREV       "RTCIS"

/* One line deployability warning, quoted on the title screen and in logs. */
#define RC_COPYRIGHT \
    "RailControl is a simulator. NOT CERTIFIED FOR REAL SIGNALLING."

/* Semantic version. RC_VERSION_BUILD is injected by the build; when it is not
 * defined we fall back to a development marker. */
#define RC_VERSION_MAJOR 1
#define RC_VERSION_MINOR 0
#define RC_VERSION_PATCH 0
#define RC_VERSION_STAGE "beta"

#ifndef RC_VERSION_BUILD
#define RC_VERSION_BUILD "dev"
#endif

#ifndef RC_BUILD_DATE
#define RC_BUILD_DATE __DATE__
#endif

#ifndef RC_BUILD_TIME
#define RC_BUILD_TIME __TIME__
#endif

#define RC_VERSION_SHORT \
    "1.0.0"

#define RC_VERSION_FULL \
    "1.0.0-beta (build " RC_VERSION_BUILD ", " RC_BUILD_DATE ")"

/* --------------------------------------------------------------------------
 * Authorship
 *
 * Stephen is the author of record for RailControl and is credited in every
 * department the program is organised into. The department list below is the
 * same list the About dialogue and the title screen print, so the credits
 * cannot drift away from the code: change the macro, change the credit.
 *
 * NOTE ON THE SAFETY ROLE
 * -----------------------
 * The safety assessor line is intentionally left as an explicit declaration
 * that no independent assessment has been carried out. On a real interlocking
 * project that name must be a signed-off, independent, competent person and
 * NOT the same person who wrote the code - EN 50128 requires the assessor to
 * be organisationally separate from the developer. Crediting the author there
 * would be a false claim, so it stays UNASSIGNED.
 * -------------------------------------------------------------------------- */
#define RC_AUTHOR_NAME       "Stephen"
#define RC_AUTHOR_HANDLE     "@Stephen"

/* Historic aliases kept so existing code and build scripts keep working. */
#define RC_DEVELOPER_NAME    RC_AUTHOR_NAME
#define RC_DEVELOPER_HANDLE  RC_AUTHOR_HANDLE
#define RC_MAINTAINER_NAME   RC_AUTHOR_NAME

#define RC_ORGANISATION "ArkansasIo"
#define RC_ORGANISATION_URL "https://github.com/ArkansasIo"
#define RC_REPOSITORY "https://github.com/ArkansasIo/server-mainframe-program"

/* Product owner and the roles a real interlocking project must staff. */
#define RC_ROLE_PRODUCT_OWNER   RC_AUTHOR_NAME
#define RC_ROLE_LEAD_ENGINEER   RC_AUTHOR_NAME
#define RC_ROLE_SAFETY_ASSESSOR "UNASSIGNED - required before any real deployment"
#define RC_ROLE_OPERATIONS      "Signaller (simulated)"

/* --------------------------------------------------------------------------
 * Department credits
 *
 * Every department Stephen is credited in. These strings are printed verbatim
 * by rc_title_screen() and rc_credits_text().
 * -------------------------------------------------------------------------- */
#define RC_DEPT_ARCHITECTURE   "System architecture"
#define RC_DEPT_INTERLOCKING   "Interlocking and safety logic"
#define RC_DEPT_SIGNALLING     "Signal control"
#define RC_DEPT_POINT_MACHINES "Point and switch control"
#define RC_DEPT_TRAIN_CONTROL  "Train control (ATC speed supervision)"
#define RC_DEPT_TRACK_CIRCUITS "Track circuits and occupancy"
#define RC_DEPT_SENSORS        "Field sensors and SCADA inputs"
#define RC_DEPT_AUTOMATION     "Automation (CONJOB rule engine)"
#define RC_DEPT_SCRIPTING      "Scripting (Lua object layer)"
#define RC_DEPT_TERMINAL       "Signaller terminal and command language"
#define RC_DEPT_GUI            "Win32 GDI dispatch console"
#define RC_DEPT_DATA           "Data and persistence"
#define RC_DEPT_SECURITY       "Accounts, roles and permissions"
#define RC_DEPT_BUILD          "Build and tooling"
#define RC_DEPT_DOCS           "Documentation and safety case"
#define RC_DEPT_TESTING        "Testing and verification"
#define RC_DEPT_DEV_OPS        "Release engineering"

/* License and support. */
#define RC_LICENSE_NAME "MIT"
#define RC_SUPPORT_CONTACT "https://github.com/ArkansasIo/server-mainframe-program/issues"

typedef struct
{
    const char *role;
    const char *name;
} RcTeamMember;

/* The team table, terminated by a NULL role. */
const RcTeamMember *rc_team(void);

/* The department credit table, terminated by a NULL department. Each entry
 * names the department and the author credited in it. */
typedef struct
{
    const char *department;
    const char *credited;
} RcCredit;

const RcCredit *rc_department_credits(void);

/* Multi-line text blocks, each with a trailing newline. */
const char *rc_version_string(void);  /* one line: name + version */
const char *rc_title_screen(void);    /* the ASCII title screen */
const char *rc_credits_text(void);    /* the About / credits block */
const char *rc_short_copyright(void); /* one line for file headers */
const char *rc_safety_notice(void);   /* the deployability warning */

/* Machine readable build banner, used as the first line of every log file. */
const char *rc_log_header(void);

#endif /* RC_VERSION_H */
