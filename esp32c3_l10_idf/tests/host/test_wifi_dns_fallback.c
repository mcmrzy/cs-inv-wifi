#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_source(void)
{
    FILE *file = fopen(WIFI_STA_SOURCE, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    char *source = malloc((size_t)size + 1);
    if (!source) {
        fclose(file);
        return NULL;
    }
    if (fread(source, 1, (size_t)size, file) != (size_t)size) {
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
    char *source = read_source();
    if (!source) return 2;

    const char *handler = strstr(source, "IP_EVENT_STA_GOT_IP");
    const char *configure = handler ? strstr(handler, "configure_dns_servers(evt->esp_netif)") : NULL;
    const char *connected = handler ? strstr(handler, "xEventGroupSetBits") : NULL;
    const char *function = strstr(source, "static void configure_dns_servers(");
    const char *primary = function ? strstr(function, "223, 5, 5, 5") : NULL;
    const char *backup = function ? strstr(function, "119, 29, 29, 29") : NULL;
    const char *fallback = function ? strstr(function, "ESP_NETIF_DNS_FALLBACK") : NULL;
    const char *set_main = function ? strstr(function, "ESP_NETIF_DNS_MAIN") : NULL;
    const char *set_backup = function ? strstr(function, "ESP_NETIF_DNS_BACKUP") : NULL;

    const int ok = configure && connected && configure < connected &&
                   function && primary && backup && fallback &&
                   set_main && set_backup;
    free(source);
    if (!ok) {
        fprintf(stderr,
                "got-IP path must install public main/backup DNS and preserve DHCP DNS as fallback before marking WiFi connected\n");
        return 1;
    }
    return 0;
}
