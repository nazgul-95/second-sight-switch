#include "language_config.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define BASE "sdmc:/switch/secondsight/"

const char *second_sight_game_directory(void)
{
    char language[16] = "en";
    struct stat info;
    FILE *file = fopen(BASE "language.txt", "r");
    if (file) {
        if (fscanf(file, "%15s", language) != 1)
            strcpy(language, "en");
        fclose(file);
    }
    if (strcmp(language, "ru") == 0) {
        /* Written only after all Russian ISO members have been extracted and
         * verified. The executable stays pinned to the identical English XBE. */
        if (stat(BASE "game/r/russian-assets.ready", &info) == 0 &&
            S_ISREG(info.st_mode)) {
            fprintf(stderr, "[LANGUAGE] selected=ru assets=" BASE "game/r\n");
            return BASE "game/r";
        }
        fprintf(stderr, "[LANGUAGE] Russian assets missing/incomplete; using English\n");
    } else if (strcmp(language, "en") != 0) {
        fprintf(stderr, "[LANGUAGE] unsupported setting; using English (expected ru or en)\n");
    }
    fprintf(stderr, "[LANGUAGE] selected=en assets=" BASE "game/e\n");
    return BASE "game/e";
}

