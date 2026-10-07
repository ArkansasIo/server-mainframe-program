/* Temporary probe: dump the real layout geometry as JSON so the map that is
 * drawn from it cannot disagree with rcp_layout.c. Not part of the build. */
#include "railway_api.h"
#include "railway_internal.h"

#include <stdio.h>

int main(void)
{
    int i;

    if (railway_init() != RW_OK)
    {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    printf("{\n");

    printf("  \"system\": \"%s\",\n", railway_system_name());
    printf("  \"signalBox\": \"%s\",\n", railway_signal_box_name());

    printf("  \"tracks\": [\n");
    for (i = 0; i < railway_track_count(); ++i)
    {
        RwTrackInfo t;
        railway_get_track_at(i, &t);
        printf("    {\"id\":%d,\"name\":\"%s\",\"speed\":%d,\"length\":%.0f,"
               "\"start\":[%.3f,%.3f],\"end\":[%.3f,%.3f]}%s\n",
               t.id, t.name, t.speed_limit_kph, (double)t.length_m,
               (double)t.start_lat, (double)t.start_lon,
               (double)t.end_lat, (double)t.end_lon,
               (i + 1 < railway_track_count()) ? "," : "");
    }
    printf("  ],\n");

    printf("  \"signals\": [\n");
    for (i = 0; i < railway_signal_count(); ++i)
    {
        RwSignalInfo s;
        railway_get_signal_at(i, &s);
        printf("    {\"id\":%d,\"name\":\"%s\",\"track\":%d,\"aspect\":\"%s\"}%s\n",
               s.id, s.name, s.track, signal_state_name(s.aspect),
               (i + 1 < railway_signal_count()) ? "," : "");
    }
    printf("  ],\n");

    printf("  \"points\": [\n");
    for (i = 0; i < railway_switch_count(); ++i)
    {
        RwSwitchInfo p;
        railway_get_switch_at(i, &p);
        printf("    {\"id\":%d,\"name\":\"%s\",\"lat\":%.3f,\"lon\":%.3f}%s\n",
               p.id, p.name, (double)p.latitude, (double)p.longitude,
               (i + 1 < railway_switch_count()) ? "," : "");
    }
    printf("  ],\n");

    printf("  \"trains\": [\n");
    for (i = 0; i < railway_train_count(); ++i)
    {
        RwTrainInfo t;
        railway_get_train_at(i, &t);
        int car_count = railway_train_car_count(t.id);
        printf("    {\"id\":%d,\"name\":\"%s\",\"track\":%d,\"cars\":%d,"
               "\"length\":%.0f,\"maxSpeed\":%.0f}%s\n",
               t.id, t.name, t.track, car_count, (double)railway_train_length_m(t.id),
               (double)t.max_speed_kph,
               (i + 1 < railway_train_count()) ? "," : "");
    }
    printf("  ],\n");

    printf("  \"routes\": [\n");
    for (i = 0; i < railway_route_count(); ++i)
    {
        RwRouteInfo r;
        railway_get_route_at(i, &r);
        printf("    {\"id\":%d,\"name\":\"%s\",\"entry\":%d,\"exit\":%d}%s\n",
               r.id, r.name, r.entry_signal, r.exit_signal,
               (i + 1 < railway_route_count()) ? "," : "");
    }
    printf("  ],\n");

    printf("  \"cars\": [\n");
    for (i = 0; i < railway_car_count(); ++i)
    {
        RwCarInfo c;
        railway_get_car_at(i, &c);
        printf("    {\"id\":%d,\"train\":%d,\"pos\":%d,\"number\":\"%s\","
               "\"type\":\"%s\",\"len\":%.1f}%s\n",
               c.id, c.train, c.position, c.number, rcp_car_type_code(c.type),
               (double)c.length_m, (i + 1 < railway_car_count()) ? "," : "");
    }
    printf("  ]\n");

    printf("}\n");
    railway_shutdown();
    return 0;
}
