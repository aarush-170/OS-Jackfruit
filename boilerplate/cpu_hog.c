#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static unsigned int parse_seconds(const char *arg, unsigned int fallback)
{
    char *end = NULL;
    unsigned long value = strtoul(arg, &end, 10);

    if (!arg || *arg == '\0' || (end && *end != '\0'))
        return fallback;

    return (unsigned int)value;
}

int main(int argc, char *argv[])
{
    unsigned int duration = 0; // 0 = infinite
    if (argc > 1)
        duration = parse_seconds(argv[1], 0);

    time_t start = time(NULL);
    time_t last_report = start;
    volatile unsigned long long accumulator = 0;

    while (1) {
        accumulator = accumulator * 1664525ULL + 1013904223ULL;

        time_t now = time(NULL);

        // Print once per second
        if (now != last_report) {
            last_report = now;
            printf("cpu_hog alive elapsed=%ld accumulator=%llu\n",
                   (long)(now - start),
                   accumulator);
            fflush(stdout);
        }

        // Exit only if duration specified
        if (duration > 0 && (unsigned int)(now - start) >= duration) {
            break;
        }
    }

    printf("cpu_hog done duration=%u accumulator=%llu\n",
           duration, accumulator);

    return 0;
}
