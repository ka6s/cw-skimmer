#include "../src/cwskimmer_api.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <capture.cwcap>\n", argv[0]);
        return 1;
    }

    return cwskimmer_replay_capture_file(argv[1]);
}