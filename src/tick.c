#include "airport.h"

int run_tick(int (*step)(int)) {
    int first_runway = airport->plane_count;
    int dispatcher_id = first_runway + airport->runway_count;
    for (int id = 0; id < first_runway; ++id) {
        if (step(id)) return -1;
    }
    for (int id = first_runway; id < dispatcher_id; ++id) {
        if (step(id)) return -1;
    }
    if (step(dispatcher_id)) return -1;
    for (int id = first_runway; id < dispatcher_id; ++id) {
        if (step(id)) return -1;
    }
    return 0;
}
