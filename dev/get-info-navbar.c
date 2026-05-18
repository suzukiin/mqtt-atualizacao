#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

int main() {

    printf("Content-Type: application/json\r\n\r\n");
    FILE *fp = fopen("../config/info.json", "r");
        if (fp == NULL) {
            printf("{\"status\": \"Error\", \"msg\": \"Não foi possível abrir o arquivo de configuração\"}\n");
        return 1;
    }

    // 2. Ler o conteúdo do arquivo para um buffer
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = (char *)malloc(size + 1);
    if (buffer == NULL) {
        fclose(fp);
        printf("{\"status\": \"Error\", \"msg\": \"Falha de alocação de memória\"}\n");
        return 1;
    }

    fread(buffer, 1, size, fp);
    buffer[size] = '\0';
    fclose(fp);

    // 3. Parsear o JSON com cJSON
    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            fprintf(stderr, "Erro antes de: %s\n", error_ptr);
        }
        printf("{\"status\": \"Error\", \"msg\": \"Erro ao processar JSON\"}\n");
        free(buffer);
        return 1;
    }

    // 4. Transformar de volta em string formatada (opcional, para teste)
    char *json_out = cJSON_Print(json);
    printf("{\"status\": \"Success\", \"data\": %s}\n", json_out);

    // Limpeza
    cJSON_Delete(json);
    free(json_out);
    free(buffer);

    return 0;
}

