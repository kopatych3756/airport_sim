#include "airport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


unsigned int seed_value = 31;
int max_ticks = 120;
int delay_ms = 0;
int emergency_fuel_ticks = 3;
int priority_weights[3] = {10, 65, 25};
int arrival_fuel_min = 9;
int arrival_fuel_max = 22;
int arrival_burn_min = 1;
int arrival_burn_max = 2;

Runway runways[MAX_RUNWAYS];
int runway_count = 0;
PlaneType types[MAX_TYPES];
int type_count = 0;
Plane planes[MAX_PLANES];
int plane_count = 0;
Closure closures[MAX_CLOSURES];
int closure_count = 0;
Flow flows[MAX_FLOWS];
int flow_count = 0;
Dispatcher dispatcher = {"ATC", 0, 0};

int current_tick = 0;
int elapsed_ticks = 0;
volatile sig_atomic_t stop_requested = 0;

static void print_help(const char *program) {
    printf("Usage: %s [options]\n", program);
    printf("  -c, --config FILE   configuration file\n");
    printf("  --seed N            override random seed\n");
    printf("  --max-ticks N       override shift length (0 = unlimited)\n");
    printf("  --delay-ms N        delay between ticks\n");
    printf("  --log FILE          log file\n");
    printf("  --no-log            do not save a log file\n");
    printf("  --quiet             do not print events to terminal\n");
    printf("  -h, --help          show help\n");
}

static void signal_handler(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

int main(int argc, char **argv) {
    const char *config_path = "configs/default.conf";
    const char *log_path = "simulation.log";
    int logging_enabled = 1;
    int quiet = 0;

    int seed_overridden = 0;
    int ticks_overridden = 0;
    int delay_overridden = 0;
    unsigned int cli_seed = 0;
    int cli_ticks = 0;
    int cli_delay = 0;

    int i;
    const char *reason;

    for (i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
            i + 1 < argc) {
            config_path = argv[++i];

        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cli_seed = (unsigned int)strtoul(argv[++i], NULL, 10);
            seed_overridden = 1;

        } else if (strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            cli_ticks = atoi(argv[++i]);
            ticks_overridden = 1;

        } else if (strcmp(argv[i], "--delay-ms") == 0 && i + 1 < argc) {
            cli_delay = atoi(argv[++i]);
            delay_overridden = 1;

        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];

        } else if (strcmp(argv[i], "--no-log") == 0) {
            logging_enabled = 0;

        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;

        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;

        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]);
            print_help(argv[0]);
            return 2;
        }
    }

    if (load_config(config_path) != 0) return 2;

    if (seed_overridden) seed_value = cli_seed;
    if (ticks_overridden) max_ticks = cli_ticks;
    if (delay_overridden) delay_ms = cli_delay;

    srand(seed_value);
    if (generate_flows() != 0) return 2;

    set_quiet_mode(quiet);
    if (logging_enabled && open_log_file(log_path) != 0) return 2;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    print_line("=== AIRPORT RUNWAY SIMULATION / VARIANT 31 ===");
    print_line("seed=%u max_ticks=%d delay_ms=%d", seed_value, max_ticks, delay_ms);
    print_line("runways=%d aircraft=%d types=%d closures=%d",
               runway_count, plane_count, type_count, closure_count);
    print_line("dispatcher=%s", dispatcher.name);
    print_line("Priority: emergency -> HIGH/NORMAL/LOW -> earlier request.");
    print_line("");

    reason = run_simulation();
    print_termination_event(reason);
    print_summary(reason);

    close_log_file();

    if (strcmp(reason, "INVARIANT_FAILURE") == 0) return 3;
    if (strcmp(reason, "USER_INTERRUPT") == 0) return 130;
    return 0;
}
