#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    FILE *file = fopen(BLE_CT_SOURCE, "rb");
    if (!file) return 2;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *source = malloc((size_t)size + 1);
    if (!source || fread(source, 1, (size_t)size, file) != (size_t)size) {
        free(source);
        fclose(file);
        return 2;
    }
    source[size] = '\0';
    fclose(file);

    const char *modules = strstr(source, "supported_upgrade_modules");
    const char *next_field = modules ? strstr(modules, "max_report_bytes") : NULL;
    const char *dsp = modules ? strstr(modules, "dsp_controller") : NULL;
    const char *bms = modules ? strstr(modules, "bms") : NULL;
    const char *binary_ota = strstr(source, "ota_binary_v1");
    const int ok = modules && next_field && dsp && bms &&
        dsp < next_field && bms < next_field &&
        binary_ota && binary_ota < modules;
    free(source);
    if (!ok) {
        fprintf(stderr, "BLE INFO must advertise binary OTA and DSP/BMS modules\n");
        return 1;
    }
    return 0;
}
