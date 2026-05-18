#include <stdio.h>
#include <sys/sysinfo.h>

int main() {
    printf("Content-Type: application/json\r\n\r\n");

    struct sysinfo info;

    if (sysinfo(&info) != 0) {
        printf("{\"status\": \"Error\", \"msg\": \"Não foi possível obter as informações de uptime\"}\n");
        return 1;
    }

    long uptime_segundos = info.uptime;

    int dias = uptime_segundos / 86400;
    int horas = (uptime_segundos % 86400) / 3600;
    int minutos = (uptime_segundos % 3600) / 60;
    int segundos = uptime_segundos % 60;

    printf("{\"status\": \"Success\", \"data\": {\"dias\": %d, \"horas\": %d, \"minutos\": %d, \"segundos\": %d}}\n", dias, horas, minutos, segundos);
    return 0;
}