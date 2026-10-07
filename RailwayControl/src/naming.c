/* ==========================================================================
 * naming.c - 🏷️ Identifier and naming system.
 *
 * Read naming.h first: this file implements the resolution order, the
 * convention checks and the uniqueness registry described there. The one
 * principle worth restating in the code itself is the resolution order,
 * because it is the safety-relevant part:
 *
 *     exact  >  case  >  alias  >  short  >  prefix  >  refuse
 *
 * A looser match never gets the chance to shadow a stricter one, and a prefix
 * that matches two assets is reported as ambiguous rather than resolved to
 * whichever happened to be first in the array. An operator who types "T1" gets
 * "T1 UP MAIN W"; an operator who types "UP MAIN" - which appears in both T1
 * and T4 - is told there are two, not silently given one of them.
 * ========================================================================== */
#include "naming.h"
#include "railway_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Kind names and prefixes
 * -------------------------------------------------------------------------- */
const char *rc_name_kind_name(RcNameKind kind)
{
    switch (kind)
    {
    case RC_NAME_KIND_TRACK:
        return "TRACK";
    case RC_NAME_KIND_SIGNAL:
        return "SIGNAL";
    case RC_NAME_KIND_POINT:
        return "POINT";
    case RC_NAME_KIND_ROUTE:
        return "ROUTE";
    case RC_NAME_KIND_TRAIN:
        return "TRAIN";
    case RC_NAME_KIND_SENSOR:
        return "SENSOR";
    case RC_NAME_KIND_CAR:
        return "CAR";
    case RC_NAME_KIND_UNKNOWN:
    case RC_NAME_KIND_COUNT:
        break;
    }
    return "UNKNOWN";
}

char rc_name_kind_prefix(RcNameKind kind)
{
    switch (kind)
    {
    case RC_NAME_KIND_TRACK:
        return 'T';
    case RC_NAME_KIND_SIGNAL:
        return 'S';
    case RC_NAME_KIND_POINT:
        return 'P';
    case RC_NAME_KIND_ROUTE:
        return 'R';
    /* A headcode has no prefix, and a sensor's prefix (TC/AC/HB/LX/TR)
     * identifies its type rather than the class, so neither gets one here. */
    default:
        return '\0';
    }
}

const char *rc_name_match_name(RcNameMatch match)
{
    switch (match)
    {
    case RC_NAME_MATCH_EXACT:
        return "exact";
    case RC_NAME_MATCH_CASE:
        return "case-insensitive";
    case RC_NAME_MATCH_ALIAS:
        return "alias";
    case RC_NAME_MATCH_SHORT:
        return "short name";
    case RC_NAME_MATCH_PREFIX:
        return "partial";
    case RC_NAME_MATCH_AMBIGUOUS:
        return "ambiguous";
    case RC_NAME_MATCH_NONE:
        break;
    }
    return "no match";
}

bool rc_name_match_is_unique(RcNameMatch match)
{
    return match == RC_NAME_MATCH_EXACT || match == RC_NAME_MATCH_CASE || match == RC_NAME_MATCH_ALIAS || match == RC_NAME_MATCH_SHORT || match == RC_NAME_MATCH_PREFIX;
}

/* --------------------------------------------------------------------------
 * Text helpers
 * -------------------------------------------------------------------------- */
static char upper_of(char c)
{
    return (char)toupper((unsigned char)c);
}

bool rc_name_equals_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }
    while (*a != '\0' && *b != '\0')
    {
        if (upper_of(*a) != upper_of(*b))
        {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

char *rc_name_normalise(const char *name, char *out, size_t size)
{
    size_t used = 0;
    bool last_was_space = false;

    if (out == NULL || size == 0)
    {
        return out;
    }
    out[0] = '\0';
    if (name == NULL)
    {
        return out;
    }

    /* Leading whitespace is ignored, runs of internal whitespace collapse to a
     * single space, and everything is upper-cased. The engine's own names are
     * already in that form, so a normalised query is directly comparable. */
    while (*name != '\0' && isspace((unsigned char)*name))
    {
        ++name;
    }

    for (; *name != '\0' && used + 1 < size; ++name)
    {
        const unsigned char c = (unsigned char)*name;
        if (isspace(c))
        {
            if (used == 0)
            {
                continue; /* no leading space in the output */
            }
            if (last_was_space)
            {
                continue; /* collapse the run */
            }
            out[used++] = ' ';
            last_was_space = true;
            continue;
        }
        out[used++] = upper_of((char)c);
        last_was_space = false;
    }

    /* Drop a trailing space left by a collapsing run. */
    if (used > 0 && out[used - 1] == ' ')
    {
        --used;
    }
    out[used] = '\0';
    return out;
}

bool rc_name_short(const char *name, char *out, size_t size)
{
    size_t used = 0;

    if (name == NULL || out == NULL || size == 0)
    {
        return false;
    }
    out[0] = '\0';

    while (*name != '\0' && isspace((unsigned char)*name))
    {
        ++name;
    }
    if (*name == '\0')
    {
        return false;
    }
    while (*name != '\0' && !isspace((unsigned char)*name) && used + 1 < size)
    {
        out[used++] = upper_of(*name);
        ++name;
    }
    out[used] = '\0';
    return used > 0;
}

bool rc_name_descriptor(const char *name, char *out, size_t size)
{
    size_t used = 0;

    if (name == NULL || out == NULL || size == 0)
    {
        return false;
    }
    out[0] = '\0';

    while (*name != '\0' && isspace((unsigned char)*name))
    {
        ++name;
    }
    /* Skip the leading token. */
    while (*name != '\0' && !isspace((unsigned char)*name))
    {
        ++name;
    }
    while (*name != '\0' && isspace((unsigned char)*name))
    {
        ++name;
    }
    if (*name == '\0')
    {
        return false;
    }
    while (*name != '\0' && used + 1 < size)
    {
        out[used++] = upper_of(*name);
        ++name;
    }
    out[used] = '\0';
    return used > 0;
}

/** True when `text` starts with `prefix`, case-insensitively. */
static bool starts_with_ci(const char *text, const char *prefix)
{
    if (text == NULL || prefix == NULL)
    {
        return false;
    }
    while (*prefix != '\0')
    {
        if (*text == '\0' || upper_of(*text) != upper_of(*prefix))
        {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

/** True when every character is a digit. */
static bool all_digits(const char *text)
{
    if (text == NULL || *text == '\0')
    {
        return false;
    }
    for (; *text != '\0'; ++text)
    {
        if (!isdigit((unsigned char)*text))
        {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------------
 * Validation
 * -------------------------------------------------------------------------- */

/* The prefix a sensor name carries in this layout. Listed here rather than
 * inferred so an unknown prefix is rejected instead of silently accepted. */
static const char *const k_sensor_prefixes[] = {
    "TC", /* track circuit */
    "AC", /* axle counter */
    "HB", /* hot box */
    "LX", /* level crossing */
    "TR", /* treadle */
    "PD", /* point detection */
    "RP", /* signal repeater */
    NULL};

const char *rc_name_validate(RcNameKind kind, const char *name)
{
    char normalised[RC_NAME_MAX];
    size_t length;

    if (name == NULL || name[0] == '\0')
    {
        return "Name must not be empty";
    }

    rc_name_normalise(name, normalised, sizeof(normalised));
    length = strlen(normalised);
    if (length == 0)
    {
        return "Name must not be blank";
    }
    if (length >= RC_NAME_MAX)
    {
        return "Name is too long (maximum 31 characters)";
    }

    switch (kind)
    {
    case RC_NAME_KIND_TRACK:
    case RC_NAME_KIND_SIGNAL:
    case RC_NAME_KIND_POINT:
    case RC_NAME_KIND_ROUTE:
    {
        /* Convention: a single letter for the class, then a number, then an
         * optional description. "T1", "S10", "R2 UP-P2". */
        const char prefix = rc_name_kind_prefix(kind);
        const char *cursor;
        size_t digits = 0;

        if (normalised[0] != prefix)
        {
            static char reason[64];
            snprintf(reason, sizeof(reason),
                     "A %s name must begin with '%c'",
                     rc_name_kind_name(kind), prefix);
            return reason;
        }
        cursor = normalised + 1;
        if (!isdigit((unsigned char)*cursor))
        {
            static char reason[64];
            snprintf(reason, sizeof(reason),
                     "A %s name must be '%c' followed by a number",
                     rc_name_kind_name(kind), prefix);
            return reason;
        }
        while (isdigit((unsigned char)*cursor))
        {
            ++cursor;
            ++digits;
        }
        if (digits == 0 || digits > 4)
        {
            return "The number in a name must be 1 to 4 digits";
        }
        /* What follows must be end-of-string or a space before a description. */
        if (*cursor != '\0' && *cursor != ' ')
        {
            return "The description after the number must be separated by a space";
        }
        return NULL;
    }

    case RC_NAME_KIND_TRAIN:
        /* A headcode is exactly four alphanumeric characters: the class digit,
         * the letter, then the two-character reporting number. */
        if (length != 4)
        {
            return "A train headcode is exactly 4 characters, e.g. 1A34";
        }
        if (!isdigit((unsigned char)normalised[0]))
        {
            return "A train headcode starts with a digit, e.g. 1A34";
        }
        if (!isalpha((unsigned char)normalised[1]))
        {
            return "The second character of a headcode is a letter, e.g. 1A34";
        }
        {
            size_t i;
            for (i = 0; i < length; ++i)
            {
                if (!isalnum((unsigned char)normalised[i]))
                {
                    return "A train headcode contains only letters and digits";
                }
            }
        }
        return NULL;

    case RC_NAME_KIND_SENSOR:
    {
        /* A sensor leads with one of the known type prefixes then a number. */
        int i;
        bool matched = false;

        for (i = 0; k_sensor_prefixes[i] != NULL; ++i)
        {
            if (starts_with_ci(normalised, k_sensor_prefixes[i]))
            {
                matched = true;
                if (all_digits(normalised + strlen(k_sensor_prefixes[i])))
                {
                    return NULL;
                }
            }
        }
        if (matched)
        {
            return "A sensor name is a type prefix then a number, e.g. TC1";
        }
        return "A sensor name must start with TC, AC, HB, LX, TR, PD or RP";
    }

    case RC_NAME_KIND_CAR:
        /* A vehicle number: 2 to 8 alphanumerics, upper or lower. */
        if (length < 2 || length > 8)
        {
            return "A car number is 2 to 8 characters";
        }
        {
            size_t i;
            for (i = 0; i < length; ++i)
            {
                if (!isalnum((unsigned char)normalised[i]))
                {
                    return "A car number contains only letters and digits";
                }
            }
        }
        return NULL;

    case RC_NAME_KIND_UNKNOWN:
    case RC_NAME_KIND_COUNT:
        break;
    }
    return "Unknown asset kind";
}

bool rc_name_is_valid(RcNameKind kind, const char *name)
{
    return rc_name_validate(kind, name) == NULL;
}

RcNameKind rc_name_kind_of(const char *name)
{
    char normalised[RC_NAME_MAX];
    char short_name[RC_NAME_MAX];

    if (name == NULL)
    {
        return RC_NAME_KIND_UNKNOWN;
    }
    rc_name_normalise(name, normalised, sizeof(normalised));
    if (normalised[0] == '\0')
    {
        return RC_NAME_KIND_UNKNOWN;
    }
    if (!rc_name_short(normalised, short_name, sizeof(short_name)))
    {
        return RC_NAME_KIND_UNKNOWN;
    }

    /* A single letter followed by digits is unambiguous. */
    if (short_name[0] != '\0' && all_digits(short_name + 1))
    {
        switch (short_name[0])
        {
        case 'T':
            return RC_NAME_KIND_TRACK;
        case 'S':
            return RC_NAME_KIND_SIGNAL;
        case 'P':
            return RC_NAME_KIND_POINT;
        case 'R':
            return RC_NAME_KIND_ROUTE;
        default:
            break;
        }
    }

    /* A sensor prefix. */
    {
        int i;
        for (i = 0; k_sensor_prefixes[i] != NULL; ++i)
        {
            if (starts_with_ci(short_name, k_sensor_prefixes[i]))
            {
                return RC_NAME_KIND_SENSOR;
            }
        }
    }

    /* A four-character headcode. */
    if (strlen(short_name) == 4 && isdigit((unsigned char)short_name[0]) && isalpha((unsigned char)short_name[1]))
    {
        return RC_NAME_KIND_TRAIN;
    }

    /* A bare car number is the last resort: no class prefix at all. */
    if (strlen(short_name) >= 2 && strlen(short_name) <= 8)
    {
        return RC_NAME_KIND_CAR;
    }
    return RC_NAME_KIND_UNKNOWN;
}

/* --------------------------------------------------------------------------
 * The registry
 * -------------------------------------------------------------------------- */
typedef struct
{
    RcNameKind kind;
    RwId id;
    char name[RC_NAME_MAX];
    char alias[RC_NAME_MAX_ALIASES][RC_NAME_MAX];
    int alias_count;
} RcNameEntry;

static RcNameEntry g_entries[RW_MAX_TRACKS + RW_MAX_SIGNALS + RW_MAX_SWITCHES + RW_MAX_ROUTES + RW_MAX_TRAINS + RW_MAX_SENSORS + RW_MAX_CARS];
static size_t g_entry_count = 0;
static bool g_built = false;

static RcNameEntry *entry_for(RcNameKind kind, RwId id)
{
    size_t i;

    for (i = 0; i < g_entry_count; ++i)
    {
        if (g_entries[i].kind == kind && g_entries[i].id == id)
        {
            return &g_entries[i];
        }
    }
    return NULL;
}

static void add_entry(RcNameKind kind, RwId id, const char *name)
{
    RcNameEntry *entry;

    if (g_entry_count >= (sizeof(g_entries) / sizeof(g_entries[0])))
    {
        return;
    }
    if (name == NULL || name[0] == '\0')
    {
        return;
    }

    entry = &g_entries[g_entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->id = id;
    rc_name_normalise(name, entry->name, sizeof(entry->name));
}

void rc_names_rebuild(void)
{
    RailwayEngine *e = rw_engine();
    size_t i;

    g_entry_count = 0;
    g_built = false;

    if (e == NULL)
    {
        return;
    }

    for (i = 0; i < e->track_count; ++i)
    {
        add_entry(RC_NAME_KIND_TRACK, e->tracks[i].id, e->tracks[i].name);
    }
    for (i = 0; i < e->signal_count; ++i)
    {
        add_entry(RC_NAME_KIND_SIGNAL, e->signals[i].id, e->signals[i].name);
    }
    for (i = 0; i < e->point_count; ++i)
    {
        add_entry(RC_NAME_KIND_POINT, e->points[i].id, e->points[i].name);
    }
    for (i = 0; i < e->route_count; ++i)
    {
        add_entry(RC_NAME_KIND_ROUTE, e->routes[i].id, e->routes[i].name);
    }
    for (i = 0; i < e->train_count; ++i)
    {
        add_entry(RC_NAME_KIND_TRAIN, e->trains[i].id, e->trains[i].headcode);
    }
    for (i = 0; i < e->sensor_count; ++i)
    {
        add_entry(RC_NAME_KIND_SENSOR, e->sensors[i].id, e->sensors[i].name);
    }
    for (i = 0; i < e->car_count; ++i)
    {
        add_entry(RC_NAME_KIND_CAR, e->cars[i].id, e->cars[i].number);
    }

    g_built = true;
}

static void ensure_built(void)
{
    if (!g_built)
    {
        rc_names_rebuild();
    }
}

RwResult rc_name_add_alias(RcNameKind kind, RwId id, const char *alias)
{
    RcNameEntry *entry;
    char normalised[RC_NAME_MAX];

    ensure_built();

    entry = entry_for(kind, id);
    if (entry == NULL)
    {
        return RW_ERR_INVALID_ID;
    }
    if (alias == NULL || alias[0] == '\0')
    {
        return RW_ERR_INVALID_ARG;
    }

    rc_name_normalise(alias, normalised, sizeof(normalised));
    if (normalised[0] == '\0')
    {
        return RW_ERR_INVALID_ARG;
    }

    /* An alias must not collide with a real name or another asset's alias:
     * that is exactly the ambiguity the whole module exists to prevent. */
    if (rc_name_is_taken(kind, normalised))
    {
        return RW_ERR_CONFLICT;
    }
    {
        size_t i;
        for (i = 0; i < g_entry_count; ++i)
        {
            int a;
            for (a = 0; a < g_entries[i].alias_count; ++a)
            {
                if (strcmp(g_entries[i].alias[a], normalised) == 0 && g_entries[i].kind == kind)
                {
                    return RW_ERR_CONFLICT;
                }
            }
        }
    }
    if (entry->alias_count >= RC_NAME_MAX_ALIASES)
    {
        return RW_ERR_RANGE;
    }

    snprintf(entry->alias[entry->alias_count], RC_NAME_MAX, "%s", normalised);
    entry->alias_count++;
    return RW_OK;
}

RwId rc_name_find_alias(RcNameKind kind, const char *alias)
{
    char normalised[RC_NAME_MAX];
    size_t i;

    ensure_built();

    if (alias == NULL)
    {
        return RW_ID_NONE;
    }
    rc_name_normalise(alias, normalised, sizeof(normalised));

    for (i = 0; i < g_entry_count; ++i)
    {
        int a;
        if (g_entries[i].kind != kind)
        {
            continue;
        }
        for (a = 0; a < g_entries[i].alias_count; ++a)
        {
            if (strcmp(g_entries[i].alias[a], normalised) == 0)
            {
                return g_entries[i].id;
            }
        }
    }
    return RW_ID_NONE;
}

bool rc_name_is_taken(RcNameKind kind, const char *name)
{
    char normalised[RC_NAME_MAX];
    size_t i;

    ensure_built();

    if (name == NULL)
    {
        return false;
    }
    rc_name_normalise(name, normalised, sizeof(normalised));

    for (i = 0; i < g_entry_count; ++i)
    {
        if (g_entries[i].kind == kind && strcmp(g_entries[i].name, normalised) == 0)
        {
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Resolution
 * -------------------------------------------------------------------------- */

/** Names of one kind, as a flat list the matcher walks. */
typedef struct
{
    RwId id;
    const char *name;
} NameRef;

static int collect(RcNameKind kind, NameRef *out, int max)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int count = 0;

    if (e == NULL)
    {
        return 0;
    }

    switch (kind)
    {
    case RC_NAME_KIND_TRACK:
        for (i = 0; i < e->track_count && count < max; ++i)
        {
            out[count].id = e->tracks[i].id;
            out[count].name = e->tracks[i].name;
            ++count;
        }
        break;
    case RC_NAME_KIND_SIGNAL:
        for (i = 0; i < e->signal_count && count < max; ++i)
        {
            out[count].id = e->signals[i].id;
            out[count].name = e->signals[i].name;
            ++count;
        }
        break;
    case RC_NAME_KIND_POINT:
        for (i = 0; i < e->point_count && count < max; ++i)
        {
            out[count].id = e->points[i].id;
            out[count].name = e->points[i].name;
            ++count;
        }
        break;
    case RC_NAME_KIND_ROUTE:
        for (i = 0; i < e->route_count && count < max; ++i)
        {
            out[count].id = e->routes[i].id;
            out[count].name = e->routes[i].name;
            ++count;
        }
        break;
    case RC_NAME_KIND_TRAIN:
        for (i = 0; i < e->train_count && count < max; ++i)
        {
            out[count].id = e->trains[i].id;
            out[count].name = e->trains[i].headcode;
            ++count;
        }
        break;
    case RC_NAME_KIND_SENSOR:
        for (i = 0; i < e->sensor_count && count < max; ++i)
        {
            out[count].id = e->sensors[i].id;
            out[count].name = e->sensors[i].name;
            ++count;
        }
        break;
    case RC_NAME_KIND_CAR:
        for (i = 0; i < e->car_count && count < max; ++i)
        {
            out[count].id = e->cars[i].id;
            out[count].name = e->cars[i].number;
            ++count;
        }
        break;
    case RC_NAME_KIND_UNKNOWN:
    case RC_NAME_KIND_COUNT:
        break;
    }
    return count;
}

static void push_candidate(RcNameResult *result, RwId id, const char *name)
{
    if (result->candidate_count >= RC_NAME_MAX_CANDIDATES)
    {
        return;
    }
    result->candidate_ids[result->candidate_count] = (int)id;
    snprintf(result->candidates[result->candidate_count], RC_NAME_MAX, "%s",
             name != NULL ? name : "");
    result->candidate_count++;
}

/* Forward declaration: the alias lookup above is reused by the resolver. */

RcNameResult rc_name_resolve(RcNameKind kind, const char *query)
{
    RcNameResult result;
    NameRef refs[RW_MAX_SENSORS + RW_MAX_CARS];
    char normalised[RC_NAME_MAX];
    char short_query[RC_NAME_MAX];
    int count;
    int i;

    memset(&result, 0, sizeof(result));
    result.kind = kind;

    if (query == NULL)
    {
        return result;
    }
    rc_name_normalise(query, normalised, sizeof(normalised));
    snprintf(result.query, sizeof(result.query), "%s", normalised);
    if (normalised[0] == '\0')
    {
        return result;
    }

    count = collect(kind, refs, (int)(sizeof(refs) / sizeof(refs[0])));
    if (count == 0)
    {
        return result;
    }

    /* ---- 1. Exact ------------------------------------------------------- */
    for (i = 0; i < count; ++i)
    {
        if (strcmp(refs[i].name, normalised) == 0)
        {
            result.id = refs[i].id;
            result.match = RC_NAME_MATCH_EXACT;
            snprintf(result.canonical, sizeof(result.canonical), "%s", refs[i].name);
            return result;
        }
    }

    /* ---- 2. Case-insensitive -------------------------------------------- */
    for (i = 0; i < count; ++i)
    {
        if (rc_name_equals_ci(refs[i].name, normalised))
        {
            result.id = refs[i].id;
            result.match = RC_NAME_MATCH_CASE;
            snprintf(result.canonical, sizeof(result.canonical), "%s", refs[i].name);
            return result;
        }
    }

    /* ---- 3. Alias ------------------------------------------------------- */
    {
        RwId alias_id = rc_name_find_alias(kind, normalised);
        if (alias_id != RW_ID_NONE)
        {
            for (i = 0; i < count; ++i)
            {
                if (refs[i].id == alias_id)
                {
                    result.id = alias_id;
                    result.match = RC_NAME_MATCH_ALIAS;
                    snprintf(result.canonical, sizeof(result.canonical), "%s",
                             refs[i].name);
                    return result;
                }
            }
        }
    }

    /* ---- 4. Short name (the leading token) ------------------------------ */
    {
        int hits = 0;
        RwId hit_id = RW_ID_NONE;
        const char *hit_name = NULL;

        rc_name_short(normalised, short_query, sizeof(short_query));
        for (i = 0; i < count; ++i)
        {
            char candidate_short[RC_NAME_MAX];
            if (!rc_name_short(refs[i].name, candidate_short, sizeof(candidate_short)))
            {
                continue;
            }
            if (strcmp(candidate_short, short_query) == 0)
            {
                if (hits == 0)
                {
                    hit_id = refs[i].id;
                    hit_name = refs[i].name;
                }
                ++hits;
                push_candidate(&result, refs[i].id, refs[i].name);
            }
        }
        if (hits == 1)
        {
            result.id = hit_id;
            result.match = RC_NAME_MATCH_SHORT;
            snprintf(result.canonical, sizeof(result.canonical), "%s", hit_name);
            return result;
        }
        /* A repeated short name that resolves to the same asset is not
         * ambiguous - it is one asset with a duplicated entry, which the
         * uniqueness check reports separately. */
        if (hits > 1)
        {
            bool all_same = true;
            for (i = 1; i < result.candidate_count; ++i)
            {
                if (result.candidate_ids[i] != result.candidate_ids[0])
                {
                    all_same = false;
                    break;
                }
            }
            if (all_same && result.candidate_count > 0)
            {
                result.id = (RwId)result.candidate_ids[0];
                result.match = RC_NAME_MATCH_SHORT;
                snprintf(result.canonical, sizeof(result.canonical), "%s",
                         result.candidates[0]);
                return result;
            }
            result.match = RC_NAME_MATCH_AMBIGUOUS;
            return result;
        }
    }

    /* ---- 5. Partial (a unique leading substring) ------------------------ */
    {
        int hits = 0;
        RwId hit_id = RW_ID_NONE;
        const char *hit_name = NULL;

        for (i = 0; i < count; ++i)
        {
            /* Only treat it as a prefix match when the query lines up with the
             * START of a word in the name, so "MAIN" matches "UP MAIN W" but a
             * mid-word fragment does not produce a surprise match. */
            char candidate[RC_NAME_MAX];
            bool word_start = true;
            size_t k;

            rc_name_normalise(refs[i].name, candidate, sizeof(candidate));

            for (k = 0; candidate[k] != '\0'; ++k)
            {
                if (word_start && starts_with_ci(candidate + k, normalised))
                {
                    if (hits == 0)
                    {
                        hit_id = refs[i].id;
                        hit_name = refs[i].name;
                    }
                    ++hits;
                    push_candidate(&result, refs[i].id, refs[i].name);
                    break;
                }
                word_start = (candidate[k] == ' ');
            }
        }

        if (hits == 1)
        {
            result.id = hit_id;
            result.match = RC_NAME_MATCH_PREFIX;
            snprintf(result.canonical, sizeof(result.canonical), "%s", hit_name);
            return result;
        }
        if (hits > 1)
        {
            result.match = RC_NAME_MATCH_AMBIGUOUS;
            result.id = RW_ID_NONE;
            return result;
        }
    }

    result.match = RC_NAME_MATCH_NONE;
    return result;
}

RcNameResult rc_name_resolve_any(const char *query)
{
    RcNameResult result;
    char normalised[RC_NAME_MAX];
    RcNameKind guessed;

    if (query == NULL)
    {
        memset(&result, 0, sizeof(result));
        return result;
    }

    rc_name_normalise(query, normalised, sizeof(normalised));

    /* If the prefix tells us the kind, use it - it is exact and cheap. */
    guessed = rc_name_kind_of(normalised);
    if (guessed != RC_NAME_KIND_UNKNOWN)
    {
        result = rc_name_resolve(guessed, normalised);
        if (result.match != RC_NAME_MATCH_NONE)
        {
            return result;
        }
    }

    /* Otherwise try every kind. An exact or case match anywhere wins; a
     * partial match in more than one kind is ambiguous across kinds. */
    {
        RcNameKind order[] = {
            RC_NAME_KIND_TRAIN, RC_NAME_KIND_SIGNAL, RC_NAME_KIND_POINT,
            RC_NAME_KIND_ROUTE, RC_NAME_KIND_TRACK, RC_NAME_KIND_SENSOR,
            RC_NAME_KIND_CAR};
        RcNameResult best;
        int loose = 0;
        size_t k;

        memset(&best, 0, sizeof(best));

        for (k = 0; k < (sizeof(order) / sizeof(order[0])); ++k)
        {
            RcNameResult candidate = rc_name_resolve(order[k], normalised);

            if (candidate.match == RC_NAME_MATCH_EXACT || candidate.match == RC_NAME_MATCH_CASE)
            {
                return candidate;
            }
            if (candidate.match == RC_NAME_MATCH_ALIAS)
            {
                return candidate;
            }
            if (candidate.match == RC_NAME_MATCH_SHORT || candidate.match == RC_NAME_MATCH_PREFIX)
            {
                if (loose == 0)
                {
                    best = candidate;
                }
                loose++;
            }
        }

        if (loose == 1)
        {
            return best;
        }
        if (loose > 1)
        {
            /* Ambiguous across types: report it rather than guess. */
            memset(&result, 0, sizeof(result));
            result.match = RC_NAME_MATCH_AMBIGUOUS;
            snprintf(result.query, sizeof(result.query), "%s", normalised);
            push_candidate(&result, best.id, best.canonical);
            return result;
        }
    }

    memset(&result, 0, sizeof(result));
    snprintf(result.query, sizeof(result.query), "%s", normalised);
    result.match = RC_NAME_MATCH_NONE;
    return result;
}

/* --------------------------------------------------------------------------
 * Uniqueness
 * -------------------------------------------------------------------------- */
int rc_names_find_collisions(RcNameCollision *out, int max)
{
    size_t i;
    size_t j;
    int found = 0;

    ensure_built();

    for (i = 0; i < g_entry_count; ++i)
    {
        for (j = i + 1; j < g_entry_count; ++j)
        {
            if (g_entries[i].kind != g_entries[j].kind)
            {
                continue;
            }
            if (strcmp(g_entries[i].name, g_entries[j].name) != 0)
            {
                continue;
            }
            if (out != NULL && found < max)
            {
                memset(&out[found], 0, sizeof(out[found]));
                out[found].kind = g_entries[i].kind;
                snprintf(out[found].name, RC_NAME_MAX, "%s", g_entries[i].name);
                out[found].first_id = g_entries[i].id;
                out[found].second_id = g_entries[j].id;
            }
            found++;
        }
    }
    return found;
}

bool rc_names_are_unique(void)
{
    return rc_names_find_collisions(NULL, 0) == 0;
}

/* --------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */
RwResult rc_names_summary(RcNamesSummary *out)
{
    size_t i;

    if (out == NULL)
    {
        return RW_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    ensure_built();

    for (i = 0; i < g_entry_count; ++i)
    {
        const RcNameEntry *entry = &g_entries[i];

        out->total++;
        if (entry->kind > RC_NAME_KIND_UNKNOWN && entry->kind < RC_NAME_KIND_COUNT)
        {
            out->per_kind[entry->kind]++;
        }
        out->aliases += entry->alias_count;
        if (!rc_name_is_valid(entry->kind, entry->name))
        {
            out->invalid++;
        }
    }

    out->collisions = rc_names_find_collisions(NULL, 0);
    return RW_OK;
}

RwResult rc_names_format(char *buffer, size_t size)
{
    size_t used = 0;
    size_t i;

    if (buffer == NULL || size == 0)
    {
        return RW_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';

    ensure_built();

    for (i = 0; i < g_entry_count; ++i)
    {
        const RcNameEntry *entry = &g_entries[i];
        int a;

        used += (size_t)snprintf(buffer + used, size - used,
                                 "%-9s %-3u %s\n",
                                 rc_name_kind_name(entry->kind),
                                 (unsigned)entry->id, entry->name);
        if (used >= size)
        {
            break;
        }

        for (a = 0; a < entry->alias_count; ++a)
        {
            used += (size_t)snprintf(buffer + used, size - used,
                                     "%-9s %-3u   alias: %s\n",
                                     "", (unsigned)entry->id, entry->alias[a]);
            if (used >= size)
            {
                break;
            }
        }
    }
    return RW_OK;
}

RwResult rc_names_format_collisions(char *buffer, size_t size)
{
    RcNameCollision collisions[16];
    int count;
    size_t used = 0;
    int i;

    if (buffer == NULL || size == 0)
    {
        return RW_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';

    count = rc_names_find_collisions(collisions, 16);
    if (count == 0)
    {
        snprintf(buffer, size, "No duplicate names: every lookup is unambiguous.\n");
        return RW_OK;
    }

    used += (size_t)snprintf(buffer + used, size - used,
                             "%d duplicate name(s) found:\n", count);
    for (i = 0; i < count && i < 16; ++i)
    {
        used += (size_t)snprintf(buffer + used, size - used,
                                 "  %-9s %-22s ids %d and %d\n",
                                 rc_name_kind_name(collisions[i].kind),
                                 collisions[i].name,
                                 (int)collisions[i].first_id,
                                 (int)collisions[i].second_id);
        if (used >= size)
        {
            break;
        }
    }
    return RW_OK;
}

/* --------------------------------------------------------------------------
 * Bridge to the engine's own lookups
 *
 * rw_signal_find() and friends are declared in railway_types.h and were pure
 * exact-match compares. They now go through the resolver, so every existing
 * caller - the terminal, the GUI, the CONJOB engine - accepts "T1" and "t1"
 * without being changed. The behaviour on an ambiguous query is to return
 * RW_ID_NONE: a caller that cannot be told which asset was meant must not be
 * given one at random.
 * -------------------------------------------------------------------------- */
RwId rc_name_resolve_id(RcNameKind kind, const char *query)
{
    RcNameResult result = rc_name_resolve(kind, query);

    if (result.match == RC_NAME_MATCH_AMBIGUOUS)
    {
        return RW_ID_NONE;
    }
    return result.id;
}
