//  main.c
//  LogFilter
//
//  Created by Pivovarchik Aleksandr on 18.12.16

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <regex.h>

typedef char bool;
#define true 1
#define false 0

#define MAX_LOG_SIZE 400
#define QUEUE_SIZE  20

// Lock-free round buffer
struct LogQueue
{
    volatile int write_pos;
    volatile int read_pos;
    volatile int filter_pos;
    // Log lines buffer
    char **buffer;
    // Filtering flags
    bool *flags;

    volatile bool is_writing_finished;
    volatile bool is_filtering_finished;
    volatile bool is_failed;
};

struct LogQueue * create_log_queue();
void free_log_queue(struct LogQueue *queue);
void increment_pos(int *pos);

char * log_search(const char *file_path, const char *mask, int max_lines, bool scan_tail, const char *separator);
void read_from_file(const char *file_path, bool scan_tail, struct LogQueue *queue);
void filter(const char *mask, int max_lines, struct LogQueue *queue);
void write_to_buffer(char **buffer, int max_lines, const char *separator, struct LogQueue *queue);

void print_help();

int main(int argc, const char **argv)
{
    if(argc != 6)
    {
        printf("Invalid arguments number, please enter 5 parameters\n");
        print_help();
        return 1;
    }
    
    const char *file_path, *mask, *separator = NULL;
    int max_lines;
    bool scan_tail;
    
    // Parse 'file_path', 'mask', 'separator'
    file_path = argv[1];
    mask = argv[2];
    separator = argv[5];

    // Parse 'max_lines'
    char *endptr;
    max_lines = strtol(argv[3], &endptr, 10);
    if(strlen(argv[3]) != (endptr - argv[3]))
    {
        printf("Invalid value of parameter 'max_lines': %s\n", argv[3]);
        print_help();
        return 1;
    }
    
    // Parse 'scan_tail'
    if(strcmp(argv[4], "true") == 0)
    {
        scan_tail = true;
    }
    else if(strcmp(argv[4], "false") == 0)
    {
        scan_tail = false;
    }
    else
    {
        printf("Invalid values of parameter 'scan_tail': %s\n", argv[4]);
        print_help();
        return 1;
    }

    printf("Processing is started\n");
    char *filtered_log = log_search(file_path, mask, max_lines, scan_tail, separator);
    if(filtered_log != NULL)
    {
        printf("%s\n", filtered_log);
    }
    printf("Processing is finished\n");
    
    return 0;
}

// Function parameters for calling in background via pthread
struct ReadFromFileParam
{
    const char *file_path;
    bool scan_tail;
    struct LogQueue *queue;
};

// Wrapper function for calling in background
void * bg_read_from_file(void *param)
{
    struct ReadFromFileParam *p = param;
    read_from_file(p->file_path, p->scan_tail, p->queue);
}

// Function parameters for calling in background via pthread
struct FilterParam
{
    const char *mask;
    int max_lines;
    struct LogQueue *queue;
};

// Wrapper function for calling in background
void * bg_filter(void *param)
{
    struct FilterParam *p = param;
    filter(p->mask, p->max_lines, p->queue);
}

char * log_search(const char *file_path, const char *mask, int max_lines, bool scan_tail, const char *separator)
{
    struct LogQueue *queue = create_log_queue();
    if(queue == NULL)
    {
        return NULL;
    }

    // Prepare parameter for executing reading from file in background
    struct ReadFromFileParam read_param;
    read_param.file_path = file_path;
    read_param.scan_tail = scan_tail;
    read_param.queue = queue;
    pthread_t read_thread_id;
    // Start child thread for reading from file 
    if(pthread_create(&read_thread_id, NULL, &bg_read_from_file, &read_param) != 0)
    {
        printf("Failed to create thread for reading from file\n");
        return NULL;
    }

    // Prepare parameter for executing filtering in background
    struct FilterParam filter_param;
    filter_param.mask = mask;
    filter_param.max_lines = max_lines;
    filter_param.queue = queue;
    pthread_t filter_thread_id;
    // Start child thread for filtering
    if(pthread_create(&filter_thread_id, NULL, &bg_filter, &filter_param) != 0)
    {
        printf("Failed to create thread for filtering\n");
        queue->is_failed = true;
        return NULL;
    }

    // Start writing to output buffer in parent thread
    char *out_buffer;
    write_to_buffer(&out_buffer, max_lines, separator, queue);

    free_log_queue(queue);

    return out_buffer;
}

// Read log lines from input file and post them into processing queue
void read_from_file(const char *file_path, bool scan_tail, struct LogQueue *queue)
{
    // Open input log file
    FILE *file = fopen(file_path, "r");
    if(file == NULL)
    {
        printf("Failed to open file: %s\n", file_path);
        queue->is_failed = true;
        return;
    }
   
    // Reading from file and write to processing queue 
    while(queue->is_failed == false && queue->is_filtering_finished == false)
    {
        if(queue->write_pos != queue->read_pos - 1 && 
                (queue->write_pos != QUEUE_SIZE - 1 || queue->read_pos != 0))
        {
            if(fgets(queue->buffer[queue->write_pos], MAX_LOG_SIZE, file) == NULL)
            {
                break;
            }
            queue->buffer[queue->write_pos][strlen(queue->buffer[queue->write_pos]) - 1] = '\0';
        
            increment_pos(&(queue->write_pos));
        }
        else
        {
            usleep(1);
        }
    }

    queue->is_writing_finished = true;
}

// Filter log lines in processign queue
void filter(const char *mask, int max_lines, struct LogQueue * queue)
{
    // Create regex pattern according to mask
    char *re_pattern = (char *)malloc(strlen(mask) * 2 + 1);
    unsigned pos = 0;
    for(unsigned i = 0; i < strlen(mask); i++)
    {
        if(mask[i] == '?')
        {
            re_pattern[pos] = '.';
        }
        else if(mask[i] == '*')
        {
            re_pattern[pos] = '.';
            re_pattern[pos + 1] = '*';
            pos++;
        }
        else
        {
            re_pattern[pos] = mask[i];
        }
        pos++;

        // Issue: need to block all other regex metasymbols with help of '\'
    }
    re_pattern[pos] = '\0';

    // Compile regex pattern
    regex_t re;
    if(regcomp(&re, re_pattern, REG_NOSUB) != 0)
    {
        printf("Failed to generate regular expression from mask\n");
        queue->is_failed = true;
        return;
    }

    // Filter log lines via regex, stop when rich maximal allowed lines number
    int passed_throw_num = 0;
    while(queue->is_failed == false && passed_throw_num < max_lines)
    {
        if(queue->filter_pos != queue->write_pos)
        {
            if(regexec(&re, queue->buffer[queue->filter_pos], 0, NULL, 0) == 0)
            {
                queue->flags[queue->filter_pos] = true;
                passed_throw_num++;
            }
            else
            {
                queue->flags[queue->filter_pos] = false;
            }

            increment_pos(&(queue->filter_pos));
        }
        else
        {
            // Stop when processed all log lines from file
            if(queue->is_writing_finished == true)
            {
                break;
            }

            usleep(1);
        }
    }

    regfree(&re);
    queue->is_filtering_finished = true;
}

// Write successfully filtered log lines from processing queue to output buffer
void write_to_buffer(char **out_buffer, int max_lines, const char *separator, struct LogQueue *queue)
{
    // Allocate output buffer
    *out_buffer = (char *)malloc(max_lines * (MAX_LOG_SIZE + strlen(separator)) + 1);
    if(*out_buffer == NULL)
    {
        printf("Failed to allocate output buffer\n");
        queue->is_failed = true;
        return;
    }
    (*out_buffer)[0] = '\0';

    // Write successfully filtered log lines to output buffer
    while(queue->is_failed == false)
    {
        if(queue->read_pos != queue->filter_pos)
        {
            if(queue->flags[queue->read_pos] == true)
            {
                strcat(*out_buffer, queue->buffer[queue->read_pos]);
                strcat(*out_buffer, separator);
            }

            increment_pos(&(queue->read_pos));
        }
        else 
        {
            // Stop when wrote all successfullty filtered log lines from file
            if(queue->is_filtering_finished == true)
            {
                break;
            }

            usleep(1);
        }
    }
}       

struct LogQueue * create_log_queue()
{
    struct LogQueue * queue = (struct LogQueue *)malloc(sizeof(struct LogQueue));
    queue->read_pos = 0;
    queue->write_pos = 0;
    queue->filter_pos = 0;
    queue->is_writing_finished = false;
    queue->is_filtering_finished = false;
    queue->is_failed = false;

    // Allocate memory for filtering flags and log lines buffers
    queue->flags = (bool *)malloc(sizeof(bool) * QUEUE_SIZE);
    queue->buffer = (char **)malloc(sizeof(char *) * QUEUE_SIZE);
    if(queue->flags == NULL || queue->buffer == NULL)
    {
        printf("Failed to allocate memory for processing queue\n");
        return NULL;
    }
    for(unsigned i = 0; i < QUEUE_SIZE; i++)
    {
        queue->buffer[i] = (char *)malloc(MAX_LOG_SIZE + 1);
        if(queue->buffer[i] == NULL)
        {
            printf("Failed to allocate memory for processing queue\n");
            return NULL;
        }
    }

    return queue;
}

void free_log_queue(struct LogQueue *queue)
{
    if(queue == NULL)
    {
        return;
    }

    for(unsigned i = 0; i < QUEUE_SIZE; i++)
    {
        free(queue->buffer[i]);
    }
    free(queue->buffer);
    free(queue->flags);
    free(queue);
}

// Increment position in round buffer
inline void increment_pos(int *pos)
{
    if(*pos == QUEUE_SIZE - 1)
    {
        *pos = 0;
    }
    else
    {
        (*pos)++;
    }
}

void print_help()
{
    printf("Run program with 5 parameters:\n\
            1) Input log file name;\n\
            2) Filtering mask (can contain * and ?);\n\
            3) Maximal number of log lines in output;\n\
            4) Reverse reading, bool: 'true' - reverse, 'false' - standard;\n\
            5) Separator\n");
}
