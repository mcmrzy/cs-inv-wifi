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
    char *source = read_source(CS_PROTOCOL_SOURCE);
    if (!source) return 2;
    const int has_resident_tx_scratch =
        strstr(source, "static uint8_t raw[CS_FRAME_TX_MAX]") != NULL;
    free(source);
    if (has_resident_tx_scratch) {
        fprintf(stderr, "cs_frame_send must not keep a 4352-byte static scratch buffer\n");
        return 1;
    }
    return 0;
}
