/* ==========================================================================
 * rc_version.c - Title screen, credits and build banners.
 * ========================================================================== */
#include "rc_version.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Team
 * -------------------------------------------------------------------------- */
static const RcTeamMember g_team[] = {
    { "Author",           RC_AUTHOR_NAME          },
    { "Product owner",    RC_ROLE_PRODUCT_OWNER   },
    { "Lead engineer",    RC_ROLE_LEAD_ENGINEER   },
    { "Developer",        RC_DEVELOPER_NAME       },
    { "Maintainer",       RC_MAINTAINER_NAME      },
    { "Operations",       RC_ROLE_OPERATIONS      },
    { "Safety assessor",  RC_ROLE_SAFETY_ASSESSOR },
    { NULL,               NULL                    }
};

const RcTeamMember *rc_team(void)
{
    return g_team;
}

/* --------------------------------------------------------------------------
 * Department credits
 *
 * Stephen is credited in every department below. This table is the single
 * source of truth for the About dialogue - the title screen renders the same
 * entries, so a new department is added once, here.
 * -------------------------------------------------------------------------- */
static const RcCredit g_credits[] = {
    { RC_DEPT_ARCHITECTURE,   RC_AUTHOR_NAME },
    { RC_DEPT_INTERLOCKING,   RC_AUTHOR_NAME },
    { RC_DEPT_SIGNALLING,     RC_AUTHOR_NAME },
    { RC_DEPT_POINT_MACHINES, RC_AUTHOR_NAME },
    { RC_DEPT_TRAIN_CONTROL,  RC_AUTHOR_NAME },
    { RC_DEPT_TRACK_CIRCUITS, RC_AUTHOR_NAME },
    { RC_DEPT_SENSORS,        RC_AUTHOR_NAME },
    { RC_DEPT_AUTOMATION,     RC_AUTHOR_NAME },
    { RC_DEPT_SCRIPTING,      RC_AUTHOR_NAME },
    { RC_DEPT_TERMINAL,       RC_AUTHOR_NAME },
    { RC_DEPT_GUI,            RC_AUTHOR_NAME },
    { RC_DEPT_DATA,           RC_AUTHOR_NAME },
    { RC_DEPT_SECURITY,       RC_AUTHOR_NAME },
    { RC_DEPT_BUILD,          RC_AUTHOR_NAME },
    { RC_DEPT_DOCS,           RC_AUTHOR_NAME },
    { RC_DEPT_TESTING,        RC_AUTHOR_NAME },
    { RC_DEPT_DEV_OPS,        RC_AUTHOR_NAME },
    { NULL,                   NULL            }
};

const RcCredit *rc_department_credits(void)
{
    return g_credits;
}

const char *rc_version_string(void)
{
    static char buffer[160];
    snprintf(buffer, sizeof(buffer), "%s %s", RC_PRODUCT_NAME, RC_VERSION_FULL);
    return buffer;
}

/* --------------------------------------------------------------------------
 * Title screen
 *
 * The banner is deliberately wide enough for an 80 column terminal and uses
 * only ASCII so it renders identically in the GUI pane, a Windows console,
 * a Unix terminal and inside a log file.
 * -------------------------------------------------------------------------- */
const char *rc_title_screen(void)
{
    static char screen[4096];
    static int built = 0;

    if (!built) {
        const RcTeamMember *member;
        size_t used = 0;

        used += (size_t)snprintf(screen + used, sizeof(screen) - used,
            "============================================================================\n"
            "                                                                            \n"
            "    ####    ###   #  #      ####   ###   #   #  #####  ####   #    ####      \n"
            "    #   #  #   #  #  #      #     #   #  ##  #    #    #   #  #    #   #     \n"
            "    ####   #####  #  #      #     #   #  # # #    #    #   #  #    #   #     \n"
            "    # #    #   #  #  #      #     #   #  #  ##    #    #   #  #    #   #     \n"
            "    #  ##  #   #  #  #####  ####   ###   #   #    #    ####   #### ####      \n"
            "                                                                            \n"
            "  %-74s\n"
            "  %-74s\n"
            "                                                                            \n"
            "============================================================================\n",
            RC_PRODUCT_SUBTITLE,
            RC_VERSION_FULL);

        used += (size_t)snprintf(screen + used, sizeof(screen) - used,
            "  Specification name   : %s (%s)\n"
            "  Classification       : CBI / EI with CTC and SCADA supervision\n"
            "  Author               : %s (%s)\n"
            "  Organisation         : %s\n"
            "  Repository           : %s\n"
            "  Built                : %s %s\n"
            "  License              : %s\n"
            "----------------------------------------------------------------------------\n"
            "  TEAM\n",
            RC_SPEC_NAME, RC_SPEC_ABBREV,
            RC_AUTHOR_NAME, RC_AUTHOR_HANDLE,
            RC_ORGANISATION,
            RC_REPOSITORY,
            RC_BUILD_DATE, RC_BUILD_TIME,
            RC_LICENSE_NAME);

        for (member = g_team; member->role != NULL; member++) {
            used += (size_t)snprintf(screen + used, sizeof(screen) - used,
                "    %-18s : %s\n", member->role, member->name);
        }

        /* Departments the author is credited in. Counted as we go so the
         * heading always agrees with the list beneath it. */
        {
            const RcCredit *credit;
            int department_count = 0;

            for (credit = g_credits; credit->department != NULL; credit++) {
                department_count++;
            }

            used += (size_t)snprintf(screen + used, sizeof(screen) - used,
                "----------------------------------------------------------------------------\n"
                "  DEPARTMENTS (%d) - all credited to %s\n",
                department_count, RC_AUTHOR_NAME);

            for (credit = g_credits; credit->department != NULL; credit++) {
                used += (size_t)snprintf(screen + used, sizeof(screen) - used,
                    "    %-38s : %s\n", credit->department, credit->credited);
            }
        }

        used += (size_t)snprintf(screen + used, sizeof(screen) - used,
            "----------------------------------------------------------------------------\n"
            "  !!! SAFETY NOTICE !!!\n"
            "  This is a SIMULATOR. It is NOT a certified interlocking and must not\n"
            "  be connected to real signalling equipment. See CREDITS for the\n"
            "  standards a deployable system would have to satisfy.\n"
            "============================================================================\n");

        built = 1;
    }
    return screen;
}

/* --------------------------------------------------------------------------
 * Credits / About text
 * -------------------------------------------------------------------------- */
const char *rc_credits_text(void)
{
    static char credits[4096];
    static int built = 0;

    if (!built) {
        const RcTeamMember *member;
        size_t used = 0;

        used += (size_t)snprintf(credits + used, sizeof(credits) - used,
            "%s\n%s\n\n", RC_PRODUCT_TITLE, RC_VERSION_FULL);

        used += (size_t)snprintf(credits + used, sizeof(credits) - used,
            "Design and implementation\n");

        for (member = g_team; member->role != NULL; member++) {
            used += (size_t)snprintf(credits + used, sizeof(credits) - used,
                "  %-18s : %s\n", member->role, member->name);
        }

        /* Department credits. Stephen is the author of record for every
         * department this program is organised into. */
        {
            const RcCredit *credit;

            used += (size_t)snprintf(credits + used, sizeof(credits) - used,
                "\n"
                "Departments - all credited to %s\n",
                RC_AUTHOR_NAME);

            for (credit = g_credits; credit->department != NULL; credit++) {
                used += (size_t)snprintf(credits + used, sizeof(credits) - used,
                    "  %-38s : %s\n", credit->department, credit->credited);
            }
        }

        used += (size_t)snprintf(credits + used, sizeof(credits) - used,
            "\n"
            "Where RailControl sits in the taxonomy\n"
            "  CBI / EI  Computer-Based / Electronic Interlocking - the route proof,\n"
            "            point locking and aspect logic in rcp_interlock.c\n"
            "  CTC       Centralised Traffic Control - one control point for the area\n"
            "  SCADA     field inputs, alarms and the event log\n"
            "  TMS       automated regulation of movements (CONJOB) and the timetable\n"
            "  ATC       speed supervision against the signalling (train controller)\n"
            "\n"
            "SAFETY NOTICE\n"
            "  %s\n"
            "\n"
            "  A deployable computer-based interlocking must additionally satisfy:\n"
            "    EN 50126  RAMS - reliability, availability, maintainability, safety\n"
            "    EN 50128  Software for railway control and protection systems\n"
            "    EN 50129  Safety-related electronic systems for signalling\n"
            "    EN 50159  Safety-related communication in transmission systems\n"
            "    IEC 61508  Functional safety, SIL 4\n"
            "\n"
            "  In particular it requires fail-safe hardware with 2-out-of-2 or\n"
            "  2-out-of-3 voting, formal verification of the interlocking truth\n"
            "  tables, independent safety assessor sign-off, a certified toolchain\n"
            "  and rigorous change control of every safety-related line of code.\n"
            "  None of those properties are claimed for this program.\n"
            "\n"
            "Support : %s\n"
            "Source  : %s\n"
            "License : %s\n",
            RC_COPYRIGHT,
            RC_SUPPORT_CONTACT,
            RC_REPOSITORY,
            RC_LICENSE_NAME);

        built = 1;
    }
    return credits;
}

const char *rc_short_copyright(void)
{
    return RC_PRODUCT_NAME " " RC_VERSION_SHORT " - "
        RC_AUTHOR_NAME " / " RC_ORGANISATION " - " RC_LICENSE_NAME;
}

const char *rc_safety_notice(void)
{
    return "SIMULATOR ONLY - not a certified interlocking, not for real signalling.";
}

const char *rc_log_header(void)
{
    static char header[512];
    snprintf(header, sizeof(header),
             "%s %s | build %s %s | author %s | %s | %s",
             RC_PRODUCT_NAME, RC_VERSION_SHORT,
             RC_VERSION_BUILD, RC_BUILD_DATE,
             RC_AUTHOR_NAME, RC_ORGANISATION, RC_LICENSE_NAME);
    return header;
}
