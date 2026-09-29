#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    FILE *file = fopen(OFFLINE_CACHE_SOURCE, "rb");
    if (!file) return 2;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *source = malloc((size_t)size + 1);
    if (!source) return 2;
    if (fread(source, 1, (size_t)size, file) != (size_t)size) return 2;
    source[size] = '\0';
    fclose(file);

    const char *process = strstr(source, "void offline_cache_process(void)");
    const char *quiet = process ? strstr(process, "if (s_quiet)") : NULL;
    const char *give = quiet ? strstr(quiet, "xSemaphoreGive(s_mutex);") : NULL;
    const char *leave = quiet ? strstr(quiet, "return;") : NULL;
    const char *add = strstr(source, "bool offline_cache_add(");
    const char *first_batch = add ? strstr(add, "if (!s_batch) {") : NULL;
    const char *alloc_failed = first_batch
        ? strstr(first_batch + 1, "if (!s_batch) {") : NULL;
    const char *alloc_give = alloc_failed
        ? strstr(alloc_failed, "xSemaphoreGive(s_mutex);") : NULL;
    const char *alloc_return = alloc_failed
        ? strstr(alloc_failed, "return false;") : NULL;
    const int quiet_ok = process && quiet && give && leave && give < leave;
    const int alloc_ok = alloc_failed && alloc_give && alloc_return &&
                         alloc_give < alloc_return;
    free(source);
    if (!quiet_ok) {
        fprintf(stderr, "quiet path must give s_mutex before returning\n");
        return 1;
    }
    if (!alloc_ok) {
        fprintf(stderr, "batch allocation failure must give s_mutex before returning\n");
        return 1;
    }
    return 0;
}
