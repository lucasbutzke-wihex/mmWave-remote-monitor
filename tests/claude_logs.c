#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>


#define MAX_LOG_SIZE (1024 * 1024) // 1 MB per file
#define MAX_BACKUPS 5
#define LOG_PATH_MAX 256

typedef struct {
    char base_path[LOG_PATH_MAX];
    FILE *fp;
    long max_size;
    int max_backups;
} RotatingLogger;

/* get current size of the log file*/
static long get_file_size(const char *path){
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return 0;
}

