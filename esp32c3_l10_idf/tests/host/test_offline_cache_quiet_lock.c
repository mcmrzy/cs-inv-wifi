#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_source(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *source = malloc((size_t)size + 1);
    if (!source || fread(source, 1, (size_t)size, file) != (size_t)size) {
        free(source);
        fclose(file);
        return NULL;
    }
    fclose(file);
    source[size] = '\0';
    return source;
}

int main(void)
{
    char *source = read_source(OFFLINE_CACHE_SOURCE);
    char *mqtt = read_source(MQTT_SOURCE);
    char *ota = read_source(OTA_SERVICE_SOURCE);
    if (!source || !mqtt || !ota) return 2;
    const char *process = strstr(source, "void offline_cache_process(void)");
    const char *quiet = process ? strstr(process, "if (offline_cache_is_quiet())") : NULL;
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
    const char *flush = strstr(source, "int offline_cache_flush_all(");
    const char *flush_guard = flush ? strstr(flush, "if (offline_cache_is_quiet()) return 0;") : NULL;
    const char *loop_guard = flush ? strstr(flush, "while (!offline_cache_is_quiet())") : NULL;
    const char *send_guard = loop_guard ? strstr(loop_guard, "if (offline_cache_is_quiet()) {") : NULL;
    const char *send = send_guard ? strstr(send_guard, "send_func(") : NULL;
    const int flush_ok = flush_guard && loop_guard && send_guard && send && send_guard < send &&
                         strstr(flush, "if (remove(path) == 0)") != NULL;
    const int gate_ok = strstr(mqtt, "if (offline_cache_is_quiet())") != NULL &&
                        strstr(ota, "if (manifest->target != OTA_TARGET_ESP) offline_cache_set_quiet(true);") != NULL &&
                        strstr(ota, "if (g_manifest.target != OTA_TARGET_ESP) offline_cache_set_quiet(false);") != NULL;
    free(source);
    free(mqtt);
    free(ota);
    if (!quiet_ok) {
        fprintf(stderr, "quiet path must give s_mutex before returning\n");
        return 1;
    }
    if (!alloc_ok) {
        fprintf(stderr, "batch allocation failure must give s_mutex before returning\n");
        return 1;
    }
    if (!flush_ok) {
        fprintf(stderr, "cache flush must stop on OTA quiet and avoid an unbounded remove retry\n");
        return 1;
    }
    if (!gate_ok) {
        fprintf(stderr, "OTA sessions must pause cache flush without stopping ARM UART\n");
        return 1;
    }
    return 0;
}
