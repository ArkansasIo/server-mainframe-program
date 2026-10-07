/* ==========================================================================
 * tools/names_tool.c - 🏷️ Identifier and naming diagnostics.
 *
 * A command-line front end for the naming system (naming.h / naming.c). It
 * exists so the naming rules can be inspected and tested without launching
 * the full console, and so a layout change can be checked before it ships:
 *
 *     names_tool --list                 every registered name, by kind
 *     names_tool --check                verify the layout's names are unique
 *     names_tool --summary              counts per kind, aliases, collisions
 *     names_tool --resolve T1           show how a query resolves
 *     names_tool --resolve-main         show how the queries used by the
 *                                       console and the demo scripts resolve
 *     names_tool --validate TRACK T9    check a name against its convention
 *     names_tool --kinds                classification of sample names
 *
 * The tool builds the real layout through railway_init(), so what it reports
 * is what the engine will actually do - not a separate model of it.
 *
 * Exit codes: 0 success, 1 a check failed, 2 initialisation failure.
 * ========================================================================== */
#include "naming.h"
#include "railway_api.h"
#include "rc_version.h"
#include "railcontrol.h"

#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Output helpers
 * -------------------------------------------------------------------------- */
static void print_rule(void)
{
    printf("------------------------------------------------------------------------------\n");
}

/** Show the result of one resolve attempt in a readable form. */
static void show_resolution(const char *query, RcNameKind kind)
{
    RcNameResult result;

    if (kind == RC_NAME_KIND_UNKNOWN)
    {
        result = rc_name_resolve_any(query);
    }
    else
    {
        result = rc_name_resolve(kind, query);
    }

    printf("  %-22s -> ", query);

    if (result.match == RC_NAME_MATCH_NONE)
    {
        printf("NO MATCH\n");
        return;
    }
    if (result.match == RC_NAME_MATCH_AMBIGUOUS)
    {
        int i;
        printf("AMBIGUOUS (%d candidates): ", result.candidate_count);
        for (i = 0; i < result.candidate_count; ++i)
        {
            printf("%s%s", i ? ", " : "", result.candidates[i]);
        }
        printf("\n");
        return;
    }

    printf("%-22s id=%-3d  (%s)\n",
           result.canonical, (int)result.id, rc_name_match_name(result.match));
}

/* --------------------------------------------------------------------------
 * Modes
 * -------------------------------------------------------------------------- */
static int mode_list(void)
{
    char buffer[16384];

    print_rule();
    printf("REGISTERED NAMES\n");
    print_rule();
    printf("%-9s %-3s %s\n", "KIND", "ID", "NAME");
    print_rule();

    if (rc_names_format(buffer, sizeof(buffer)) != RW_OK)
    {
        printf("(could not format the registry)\n");
        return 1;
    }
    printf("%s", buffer);
    return 0;
}

static int mode_summary(void)
{
    RcNamesSummary summary;
    int kind;

    if (rc_names_summary(&summary) != RW_OK)
    {
        printf("(could not summarise the registry)\n");
        return 1;
    }

    print_rule();
    printf("NAMING SUMMARY\n");
    print_rule();
    printf("  Total registered names : %d\n", summary.total);
    for (kind = 1; kind < RC_NAME_KIND_COUNT; ++kind)
    {
        if (summary.per_kind[kind] > 0)
        {
            printf("    %-9s            : %d\n",
                   rc_name_kind_name((RcNameKind)kind), summary.per_kind[kind]);
        }
    }
    printf("  Aliases                : %d\n", summary.aliases);
    printf("  Duplicate names        : %d\n", summary.collisions);
    printf("  Off-convention names   : %d\n", summary.invalid);
    print_rule();

    return summary.collisions == 0 ? 0 : 1;
}

static int mode_check(void)
{
    char buffer[8192];
    const bool unique = rc_names_are_unique();

    print_rule();
    printf("UNIQUENESS CHECK\n");
    print_rule();

    if (rc_names_format_collisions(buffer, sizeof(buffer)) != RW_OK)
    {
        printf("(could not check the registry)\n");
        return 1;
    }
    printf("%s", buffer);
    print_rule();

    if (unique)
    {
        printf("PASS: every name resolves to exactly one asset.\n");
        return 0;
    }
    printf("FAIL: duplicate names make some lookups ambiguous.\n");
    return 1;
}

static int mode_validate(int argc, char **argv)
{
    int i;
    int failures = 0;
    int checked = 0;

    print_rule();
    printf("NAME CONVENTION CHECK\n");
    print_rule();

    if (argc < 3)
    {
        printf("usage: names_tool --validate <TRACK|SIGNAL|POINT|ROUTE|TRAIN|SENSOR|CAR> <name> [...]\n");
        return 1;
    }

    for (i = 2; i < argc; ++i)
    {
        const char *kind_name = argv[1];
        const char *name = argv[i];
        RcNameKind kind = RC_NAME_KIND_UNKNOWN;
        const char *reason;
        int k;

        for (k = 1; k < RC_NAME_KIND_COUNT; ++k)
        {
            if (rc_name_equals_ci(rc_name_kind_name((RcNameKind)k), kind_name))
            {
                kind = (RcNameKind)k;
                break;
            }
        }
        if (kind == RC_NAME_KIND_UNKNOWN)
        {
            printf("  unknown kind '%s'\n", kind_name);
            return 1;
        }

        reason = rc_name_validate(kind, name);
        checked++;
        if (reason == NULL)
        {
            printf("  %-9s %-22s OK\n", kind_name, name);
        }
        else
        {
            printf("  %-9s %-22s REJECTED: %s\n", kind_name, name, reason);
            failures++;
        }
    }

    print_rule();
    printf("%d checked, %d rejected.\n", checked, failures);
    return failures == 0 ? 0 : 1;
}

static int mode_kinds(void)
{
    static const char *samples[] = {
        "T1 UP MAIN W", "T12 GOODS LOOP", "S3", "P1", "R1 UP-P1-EAST",
        "1A34", "4E71", "TC1", "AC1", "HB1", "LX1", "TR1", "43021", NULL};
    int i;

    print_rule();
    printf("NAME CLASSIFICATION\n");
    print_rule();
    printf("%-18s %-9s %s\n", "NAME", "KIND", "SHORT / DESCRIPTION");
    print_rule();

    for (i = 0; samples[i] != NULL; ++i)
    {
        char short_name[RC_NAME_MAX];
        char descriptor[RC_NAME_MAX];
        const RcNameKind kind = rc_name_kind_of(samples[i]);

        rc_name_short(samples[i], short_name, sizeof(short_name));
        if (!rc_name_descriptor(samples[i], descriptor, sizeof(descriptor)))
        {
            snprintf(descriptor, sizeof(descriptor), "-");
        }
        printf("%-18s %-9s %s / %s\n", samples[i],
               rc_name_kind_name(kind), short_name, descriptor);
    }
    return 0;
}

static int mode_resolve(int argc, char **argv)
{
    int i;

    print_rule();
    printf("RESOLUTION\n");
    print_rule();
    printf("  Query                  Result\n");
    print_rule();

    if (argc < 2)
    {
        printf("usage: names_tool --resolve <name> [<name> ...]\n");
        return 1;
    }

    for (i = 1; i < argc; ++i)
    {
        show_resolution(argv[i], RC_NAME_KIND_UNKNOWN);
    }
    return 0;
}

/**
 * Resolve the exact query forms that the console and the demo scripts use.
 * This is the regression check: these are the strings an operator or a batch
 * file actually types, so if one of them stops resolving, a documented
 * command has silently broken.
 */
static int mode_resolve_main(void)
{
    static const struct
    {
        const char *query;
        RcNameKind kind;
        const char *note;
    } cases[] = {
        {"S3", RC_NAME_KIND_SIGNAL, "exact signal name"},
        {"s3", RC_NAME_KIND_SIGNAL, "signal, lower case"},
        {"P1", RC_NAME_KIND_POINT, "exact point name"},
        {"R2", RC_NAME_KIND_ROUTE, "route by short name"},
        {"T1", RC_NAME_KIND_TRACK, "track by short name"},
        {"1A34", RC_NAME_KIND_TRAIN, "train headcode"},
        {"4E71", RC_NAME_KIND_TRAIN, "freight headcode"},
        {"TC1", RC_NAME_KIND_SENSOR, "track circuit sensor"},
        {"HB1", RC_NAME_KIND_SENSOR, "hot box sensor"},
        {"UP", RC_NAME_KIND_TRACK, "partial, expected ambiguous"},
        {"NOPE", RC_NAME_KIND_TRACK, "unknown name"},
        {NULL, RC_NAME_KIND_UNKNOWN, NULL}};
    int i;
    int problems = 0;

    print_rule();
    printf("RESOLUTION OF THE COMMAND-LINE FORMS\n");
    print_rule();
    printf("  %-10s %-26s %s\n", "QUERY", "RESULT", "NOTE");
    print_rule();

    for (i = 0; cases[i].query != NULL; ++i)
    {
        RcNameResult result = rc_name_resolve(cases[i].kind, cases[i].query);
        char outcome[64];

        if (result.match == RC_NAME_MATCH_NONE)
        {
            snprintf(outcome, sizeof(outcome), "no match");
        }
        else if (result.match == RC_NAME_MATCH_AMBIGUOUS)
        {
            snprintf(outcome, sizeof(outcome), "ambiguous (%d)", result.candidate_count);
        }
        else
        {
            snprintf(outcome, sizeof(outcome), "%s", result.canonical);
        }

        printf("  %-10s %-26s %s\n", cases[i].query, outcome, cases[i].note);

        /* The two negative cases must NOT resolve; every positive one must. */
        if (strcmp(cases[i].query, "UP") == 0 || strcmp(cases[i].query, "NOPE") == 0)
        {
            if (result.match != RC_NAME_MATCH_AMBIGUOUS && result.match != RC_NAME_MATCH_NONE)
            {
                problems++;
            }
        }
        else if (result.match == RC_NAME_MATCH_NONE || result.match == RC_NAME_MATCH_AMBIGUOUS)
        {
            problems++;
        }
    }

    print_rule();
    if (problems == 0)
    {
        printf("PASS: every documented command form resolves as expected.\n");
        return 0;
    }
    printf("FAIL: %d command form(s) did not resolve as expected.\n", problems);
    return 1;
}

static void print_usage(void)
{
    printf("usage: names_tool [mode] [args]\n\n");
    printf("  --list                every registered name, grouped by kind\n");
    printf("  --summary             counts per kind, aliases and collisions\n");
    printf("  --check               verify no two assets share a name\n");
    printf("  --resolve <name>...   show how a query resolves\n");
    printf("  --resolve-main        resolve the documented command forms\n");
    printf("  --validate <kind> <name>...  check names against their convention\n");
    printf("  --kinds               classify sample names by prefix\n");
    printf("  --version             version and exit\n");
    printf("  --help                this text\n\n");
    printf("With no mode, --summary and --check are both run.\n");
}

int main(int argc, char **argv)
{
    int i;
    int result;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
        {
            printf("%s\n", rc_version_string());
            printf("%s\n", rc_short_copyright());
            return 0;
        }
    }

    if (railway_init() != RW_OK)
    {
        fprintf(stderr, "Failed to initialise the railway engine.\n");
        return 2;
    }

    /* Build the registry from the live layout before anything is inspected. */
    rc_names_rebuild();

    if (argc <= 1)
    {
        result = mode_summary();
        printf("\n");
        if (mode_check() != 0)
        {
            result = 1;
        }
        railway_shutdown();
        return result;
    }

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--list") == 0)
        {
            result = mode_list();
        }
        else if (strcmp(argv[i], "--summary") == 0)
        {
            result = mode_summary();
        }
        else if (strcmp(argv[i], "--check") == 0)
        {
            result = mode_check();
        }
        else if (strcmp(argv[i], "--kinds") == 0)
        {
            result = mode_kinds();
        }
        else if (strcmp(argv[i], "--resolve") == 0)
        {
            result = mode_resolve(argc - i, argv + i);
            i = argc;
        }
        else if (strcmp(argv[i], "--resolve-main") == 0)
        {
            result = mode_resolve_main();
        }
        else if (strcmp(argv[i], "--validate") == 0)
        {
            /* The kind is the next argument, so pass the remainder through. */
            result = mode_validate(argc - i, argv + i);
            i = argc;
        }
        else
        {
            printf("Unknown option '%s'\n\n", argv[i]);
            print_usage();
            railway_shutdown();
            return 1;
        }

        if (result != 0)
        {
            railway_shutdown();
            return result;
        }
    }

    railway_shutdown();
    return 0;
}
