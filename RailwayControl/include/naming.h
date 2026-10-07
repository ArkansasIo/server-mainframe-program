/* ==========================================================================
 * naming.h - 🏷️ Identifier and naming system.
 *
 * Every asset in the engine has a name, and those names follow conventions a
 * signaller already knows:
 *
 *     track    T1 UP MAIN W        T<n> then a description
 *     signal   S3                  S<n>
 *     point    P1                  P<n>
 *     route    R1 UP-P1-EAST       R<n> then a description
 *     train    1A34                4-character headcode
 *     sensor   TC1, AC1, LX1       a type prefix then a number
 *
 * The engine has always had those names, but until now the ONLY way to refer
 * to an asset was to type the whole string exactly, as it was spelled in
 * rcp_layout.c. That is a poor interface for a person under pressure and a
 * worse one for a script: "T1" is unambiguous, yet it used to be rejected.
 *
 * This module is the layer that turns an operator's input into an asset, and
 * it exists for three reasons:
 *
 *   1. RESOLUTION. An operator types what they see on the panel - "T1", "t1",
 *      "UP MAIN W" - and the resolver finds the asset. Resolution is ordered
 *      from strictest to loosest (see RcNameMatch), and the order is the whole
 *      safety argument: an exact match always wins, a fuzzy guess never
 *      silently shadows one.
 *
 *   2. VALIDATION. A name an operator invents must follow the same convention
 *      as the built-in layout, or the next person to read the log cannot tell
 *      what "FIDO" refers to. RcName validation rejects a name that breaks its
 *      type's convention before it enters the engine.
 *
 *   3. UNIQUENESS. The engine stores names in fixed arrays and looks them up
 *      by string compare. Two assets sharing a name makes every lookup
 *      ambiguous, so the registry reports duplicates rather than letting the
 *      first one silently win.
 *
 * DESIGN NOTE ON AMBIGUITY
 * ------------------------
 * A resolver that guesses is dangerous. If "T1" could match two assets the
 * correct behaviour is to refuse and say so, not to pick one. Every loose
 * match therefore reports the list of candidates it found, and the caller can
 * tell the operator which one they meant. `RcNameMatch` reports how the match
 * was made so a front end can say "matched T1 UP MAIN W (by short name)"
 * instead of pretending the operator typed the full name.
 *
 * SAFETY NOTE
 * -----------
 * Names are labelling, not safety logic. Nothing here decides whether a route
 * is safe; it only decides which asset the operator meant. That separation
 * matters: a name collision must never be able to alter an interlocking
 * decision, only to make a command refuse.
 * ========================================================================== */
#ifndef RC_NAMING_H
#define RC_NAMING_H

#include "railway_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RC_NAME_MAX RW_MAX_NAME  /* 32: matches the engine's fields */
#define RC_NAME_MAX_ALIASES 8    /* extra names one asset may carry */
#define RC_NAME_MAX_CANDIDATES 8 /* reported on an ambiguous match */

    /* --------------------------------------------------------------------------
     * What kind of asset a name refers to. Mirrors the engine's entity types.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        RC_NAME_KIND_UNKNOWN = 0,
        RC_NAME_KIND_TRACK,
        RC_NAME_KIND_SIGNAL,
        RC_NAME_KIND_POINT,
        RC_NAME_KIND_ROUTE,
        RC_NAME_KIND_TRAIN,
        RC_NAME_KIND_SENSOR,
        RC_NAME_KIND_CAR,
        RC_NAME_KIND_COUNT
    } RcNameKind;

    const char *rc_name_kind_name(RcNameKind kind);

    /** The single-letter prefix the convention uses, or '\0' for none.
     *  track 'T', signal 'S', point 'P', route 'R'; trains and sensors have no
     *  single-letter prefix (a headcode is all digits/letters, a sensor has its
     *  own two-letter prefix). */
    char rc_name_kind_prefix(RcNameKind kind);

    /* --------------------------------------------------------------------------
     * How a name was matched. Ordered strictest first - the order IS the policy.
     * -------------------------------------------------------------------------- */
    typedef enum
    {
        RC_NAME_MATCH_NONE = 0, /* nothing matched */
        RC_NAME_MATCH_EXACT,    /* the full canonical name, character for character */
        RC_NAME_MATCH_CASE,     /* the full name, differing only in case */
        RC_NAME_MATCH_ALIAS,    /* one of the asset's declared aliases */
        RC_NAME_MATCH_SHORT,    /* the leading token, e.g. "T1" for "T1 UP MAIN W" */
        RC_NAME_MATCH_PREFIX,   /* a unique leading substring, e.g. "UP MAIN" */
        RC_NAME_MATCH_AMBIGUOUS /* two or more candidates: refuse, and say so */
    } RcNameMatch;

    const char *rc_name_match_name(RcNameMatch match);

    /** True when the result identifies exactly one asset. */
    bool rc_name_match_is_unique(RcNameMatch match);

    /* --------------------------------------------------------------------------
     * Result of a resolve attempt.
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        RwId id; /* the asset, or RW_ID_NONE */
        RcNameKind kind;
        RcNameMatch match;
        char canonical[RC_NAME_MAX]; /* the asset's full name */
        char query[RC_NAME_MAX];     /* what the operator typed, normalised */

        /* Populated only for RC_NAME_MATCH_AMBIGUOUS: up to
         * RC_NAME_MAX_CANDIDATES names the caller can show the operator. */
        char candidates[RC_NAME_MAX_CANDIDATES][RC_NAME_MAX];
        int candidate_ids[RC_NAME_MAX_CANDIDATES];
        int candidate_count;
    } RcNameResult;

    /* --------------------------------------------------------------------------
     * Validation
     * -------------------------------------------------------------------------- */

    /* Check a name against its type's convention.
     * Returns NULL when acceptable, otherwise a static reason string. */
    const char *rc_name_validate(RcNameKind kind, const char *name);

    /** True when the name is well formed for its kind. */
    bool rc_name_is_valid(RcNameKind kind, const char *name);

    /** Normalise into `out`: trim, collapse runs of spaces, upper-case. The
     *  engine's names are upper-case with single spaces, so this is what a typed
     *  name is compared against. Returns `out`. */
    char *rc_name_normalise(const char *name, char *out, size_t size);

    /** Extract the leading token of a name: "T1 UP MAIN W" -> "T1".
     *  Returns false when the name has no leading token. */
    bool rc_name_short(const char *name, char *out, size_t size);

    /** Extract the descriptive part: "T1 UP MAIN W" -> "UP MAIN W".
     *  Returns false when there is nothing after the leading token. */
    bool rc_name_descriptor(const char *name, char *out, size_t size);

    /** Classify a name's kind by its prefix alone: "S3" -> SIGNAL.
     *  Ambiguous or unrecognised prefixes return RC_NAME_KIND_UNKNOWN. */
    RcNameKind rc_name_kind_of(const char *name);

    /* --------------------------------------------------------------------------
     * Resolution
     *
     * These search the live engine. They do not require the registry to be built
     * first, so an early caller still gets a useful answer.
     * -------------------------------------------------------------------------- */

    /** Resolve a name of a known kind. */
    RcNameResult rc_name_resolve(RcNameKind kind, const char *query);

    /** Resolve against every kind and report which one matched. Used where the
     *  operator has not said what they are referring to (the command palette, a
     *  "GO TO <name>" box). */
    RcNameResult rc_name_resolve_any(const char *query);

    /** Resolve straight to an id, for callers that can only take an id.
     *  Returns RW_ID_NONE when nothing matched OR when the match was ambiguous -
     *  a caller that cannot be told which asset was meant must not be given one. */
    RwId rc_name_resolve_id(RcNameKind kind, const char *query);

    /** Case-insensitive string compare, exposed because the terminal and the name
     *  validators need the same one. */
    bool rc_name_equals_ci(const char *a, const char *b);

    /* --------------------------------------------------------------------------
     * The registry
     *
     * Built once after the layout exists. It is what makes uniqueness checking and
     * alias lookup possible: the engine's own arrays cannot answer "is this name
     * already used anywhere" without walking every type.
     * -------------------------------------------------------------------------- */

    /* Rebuild the registry from the live engine. Safe to call again. */
    void rc_names_rebuild(void);

    /** Register an alias for an asset, so an operator's local shorthand resolves.
     *  Refused when the alias is already taken by a different asset. */
    RwResult rc_name_add_alias(RcNameKind kind, RwId id, const char *alias);

    /** Look up an alias exactly. RW_ID_NONE when it is not registered. */
    RwId rc_name_find_alias(RcNameKind kind, const char *alias);

    /** True when the name is already used by an asset of this kind. */
    bool rc_name_is_taken(RcNameKind kind, const char *name);

    /* --------------------------------------------------------------------------
     * Uniqueness reporting
     *
     * The engine stores names in parallel arrays and looks them up by string
     * compare, so a duplicate makes every lookup ambiguous. The registry reports
     * them instead of letting the first match silently win - which is what makes
     * a collision a visible configuration error rather than a confusing bug.
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        RcNameKind kind;
        char name[RC_NAME_MAX];
        RwId first_id;
        RwId second_id;
    } RcNameCollision;

    /** Find assets that share a name. Returns the number found, and fills up to
     *  `max` entries. A count above `max` means more exist than were returned. */
    int rc_names_find_collisions(RcNameCollision *out, int max);

    /** True when every name in the engine is unique. Called after the layout is
     *  built; a false result is written to the event log as a configuration
     *  fault, because every one of those lookups is now a coin toss. */
    bool rc_names_are_unique(void);

    /* --------------------------------------------------------------------------
     * Diagnostics
     * -------------------------------------------------------------------------- */
    typedef struct
    {
        int total;
        int per_kind[RC_NAME_KIND_COUNT];
        int aliases;
        int collisions;
        int invalid;
    } RcNamesSummary;

    RwResult rc_names_summary(RcNamesSummary *out);

    /** Render every registered name, one per line, for the About / diagnostics
     *  panel and the `NAMES` terminal command. */
    RwResult rc_names_format(char *buffer, size_t size);

    /** Render the collisions, for the `NAMES CHECK` command. */
    RwResult rc_names_format_collisions(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RC_NAMING_H */
