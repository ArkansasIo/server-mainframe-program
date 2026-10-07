/* ==========================================================================
 * rcp_eventlog.c - 📝 Event and alarm log.
 *
 * A bounded ring of EventRecord entries. When the buffer is full the oldest
 * entry is dropped, but a critical event is never silently discarded: the
 * count of dropped records is surfaced through the status line instead.
 *
 * Severity discipline:
 *   INFO      routine operation (route set, train departed)
 *   WARNING   something the signaller should look at (signal replaced,
 *             point released under a route)
 *   ALARM     something unsafe or degraded (track circuit failure, train
 *             passing a signal at danger)
 *   CRITICAL  the interlocking refused to protect itself, or an emergency
 *             stop was tripped
 * ========================================================================== */
#include "railway_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void rw_event_log_init(void)
{
    RailwayEngine *e = rw_engine();

    if (e == NULL)
    {
        return;
    }
    memset(e->events, 0, sizeof(e->events));
    e->event_count = 0;
    e->next_event_id = 1;
}

void rw_event(RwSeverity severity, const char *category, const char *format, ...)
{
    RailwayEngine *e = rw_engine();
    EventRecord *slot;
    va_list args;

    if (e == NULL || !e->initialized || format == NULL)
    {
        return;
    }

    if (e->event_count >= RW_MAX_EVENTS)
    {
        /* Drop the oldest record to make room. Shift once; the buffer is a
         * linear array and this only happens at the limit. */
        memmove(&e->events[0], &e->events[1],
                sizeof(EventRecord) * (RW_MAX_EVENTS - 1));
        e->event_count = RW_MAX_EVENTS - 1;
    }

    slot = &e->events[e->event_count++];
    memset(slot, 0, sizeof(*slot));

    slot->id = e->next_event_id++;
    if (slot->id == RW_ID_NONE)
    {
        slot->id = e->next_event_id++; /* never hand out id 0 */
    }
    slot->severity = severity;
    slot->acknowledged = false;

    snprintf(slot->category, sizeof(slot->category), "%s",
             (category != NULL) ? category : "GENERAL");

    va_start(args, format);
    vsnprintf(slot->message, sizeof(slot->message), format, args);
    va_end(args);

    rw_now_string(slot->timestamp, sizeof(slot->timestamp));

    /* Failures always find their way to stderr too, so a headless run leaves
     * a trace even if nobody is watching the console window. */
    if (severity >= SEVERITY_ALARM)
    {
        fprintf(stderr, "[%s] %-8s %-12s %s\n",
                slot->timestamp, severity_name(severity), slot->category, slot->message);
        fflush(stderr);
    }
}

int rw_event_severity_count(RwSeverity severity)
{
    RailwayEngine *e = rw_engine();
    size_t i;
    int count = 0;

    if (e == NULL)
    {
        return 0;
    }
    for (i = 0; i < e->event_count; ++i)
    {
        if (e->events[i].severity == severity)
        {
            count++;
        }
    }
    return count;
}
