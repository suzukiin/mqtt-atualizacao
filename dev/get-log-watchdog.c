#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main()
{

    printf("Content-Type: application/json\r\n\r\n");

    FILE *fp = fopen("/var/log/watchdog.log", "r"); // Ajuste o caminho do seu log
    if (fp == NULL)
    {
        printf("{\"status\": \"Error\", \"msg\": \"Nao foi possivel abrir o log\"}\n");
        return 1;
    }

    printf("{\"status\": \"Success\", \"logs\": [");

    char line[512];
    int first = 1;

    while (fgets(line, sizeof(line), fp))
    {
        // Remove a quebra de linha se existir
        line[strcspn(line, "\n")] = 0;

        if (!first)
        {
            printf(",");
        }

        printf("\"%s\"", line);
        first = 0;
    }

    printf("]} \n");

    fclose(fp);
    return 0;
}