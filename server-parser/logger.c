#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#include "logger.h"

#define MAX_LOG_MESSAGE 512

typedef struct LogNode{
    char message[MAX_LOG_MESSAGE];
    struct LogNode *next;
} LogNode;

static LogLevel current_level = LOG_INFO;

static pthread_t logger_thread;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

static LogNode *head = NULL;
static LogNode *tail = NULL;

static bool running = true;

static int fd = -1;

static char logfile[256];

static size_t current_size = 0;

static size_t max_log_size = 0;

static int max_log_backups = 0;


void logger_set_level(LogLevel level)
{
    current_level = level;
}

static const char *level_string(LogLevel level)
{
    switch(level)
    {
        case LOG_TRACE: return "TRACE";
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        case LOG_FATAL: return "FATAL";
    }

    return "UNKNOWN";
}

static void rotate_logs(void)
{
    close(fd);

    char old_name[256];
    char new_name[256];

    for (int i=max_log_backups-1;i>=1;i--)
    {
        sprintf(old_name,"%s.%d",logfile,i);
        sprintf(new_name,"%s.%d",logfile,i+1);

        rename(old_name,new_name);
    }

    sprintf(new_name,"%s.1",logfile);

    rename(logfile,new_name);

    fd = open(logfile,
              O_CREAT|O_APPEND|O_WRONLY,
              0644);

    current_size = 0;
}

static void *logger_worker(void *arg)
{
    (void)arg;

    while (1)
    {
        pthread_mutex_lock(&mutex);

        while (head == NULL && running)
            pthread_cond_wait(&cond,&mutex);

        if (!running && head==NULL)
        {
            pthread_mutex_unlock(&mutex);
            break;
        }

        LogNode *node = head;

        head = node->next;

        if(head==NULL)
            tail=NULL;

        pthread_mutex_unlock(&mutex);

        size_t len = strlen(node->message);

        write(fd,node->message,len);

        write(fd,"\n",1);

        current_size += len+1;

        free(node);

        if(current_size >= max_log_size)
            rotate_logs();
    }

    close(fd);

    return NULL;
}

int logger_init(const char *filename,
                size_t max_size,
                int backups)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    strcpy(logfile,filename);

    max_log_size = max_size;

    max_log_backups = backups;

    fd = open(filename,
              O_CREAT|O_APPEND|O_WRONLY,
              0644);

    if(fd<0)
        return -1;

    struct stat st;

    if(stat(filename,&st)==0)
        current_size = st.st_size;

    pthread_create(
            &logger_thread,
            NULL,
            logger_worker,
            NULL);

    return 0;
}

void logger_log(LogLevel level,
                const char *fmt,
                ...)
{
    if(level < current_level)
        return;

    char buffer[MAX_LOG_MESSAGE];

    va_list args;

    va_start(args,fmt);

    vsnprintf(buffer,sizeof(buffer),fmt,args);

    va_end(args);

    LogNode *node = malloc(sizeof(LogNode));

    if(node==NULL)
        return;

    strcpy(node->message,buffer);

    node->next=NULL;

    pthread_mutex_lock(&mutex);

    if(tail)
        tail->next=node;
    else
        head=node;

    tail=node;

    pthread_cond_signal(&cond);

    pthread_mutex_unlock(&mutex);
}

void logger_shutdown()
{
    pthread_mutex_lock(&mutex);

    running=0;

    pthread_cond_signal(&cond);

    pthread_mutex_unlock(&mutex);

    pthread_join(logger_thread,NULL);
}
