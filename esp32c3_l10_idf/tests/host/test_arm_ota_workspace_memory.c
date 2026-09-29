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
    source[size] = '\0';
    fclose(file);
    return source;
}

int main(void)
{
    char *source = read_source(ARM_OTA_SOURCE);
    char *service = read_source(OTA_SERVICE_SOURCE);
    if (!source || !service) {
        free(source);
        free(service);
        return 2;
    }
    const int has_duplicate_packet =
        strstr(source, "uint8_t  packet[ARM_IAP_FIRST_PAYLOAD_SIZE]") != NULL;
    const char *compute_crc = strstr(source, "static bool iap_compute_chain_crc(");
    const char *null_guard = compute_crc
        ? strstr(compute_crc, "if (!s_ws) return false;") : NULL;
    const char *workspace_use = compute_crc
        ? strstr(compute_crc, "s_ws->iap_data") : NULL;
    const int guard_ok = null_guard && workspace_use && null_guard < workspace_use;
    const int storage_result_ok =
        strstr(source, "arm_ota_last_error_is_storage") != NULL &&
        strstr(service, "OTA_RESULT_INSUFFICIENT_STORAGE") != NULL &&
        strstr(service, "arm_ota_last_error_is_storage()") != NULL;
    free(source);
    free(service);
    if (has_duplicate_packet) {
        fprintf(stderr, "ARM OTA workspace must build packets directly into iap_data\n");
        return 1;
    }
    if (!guard_ok) {
        fprintf(stderr, "chain CRC must reject a missing workspace before dereference\n");
        return 1;
    }
    if (!storage_result_ok) {
        fprintf(stderr, "cache reservation failure must have a distinct OTA result\n");
        return 1;
    }
    return 0;
}
