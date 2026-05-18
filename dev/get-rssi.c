#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

int main() {
    int fd;
    char buffer[512];
    struct termios options;

    printf("Content-Type: application/json\r\n\r\n");

    fd = open("/dev/ttyUSB1", O_RDWR | O_NOCTTY); // Removi o O_NDELAY para ele esperar os dados
    if (fd == -1) {
        printf("{\"status\": \"Error\", \"msg\": \"Erro ao abrir porta\"}\n");
        return 1;
    }

    // Configuração padrão de porta serial
    tcgetattr(fd, &options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_cflag |= (CLOCAL | CREAD | CS8);
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // Modo RAW
    options.c_oflag &= ~OPOST;
    options.c_cc[VMIN] = 0;  // Não bloqueia a leitura
    options.c_cc[VTIME] = 10; // Timeout de 1 segundo (10 * 0.1s)
    tcsetattr(fd, TCSANOW, &options);

    // 1. Limpar sujeira residual
    tcflush(fd, TCIOFLUSH);

    // 2. Opcional: Desligar o ECO para facilitar a vida
    write(fd, "ATE0\r\n", 6);
    usleep(100000); 
    tcflush(fd, TCIFLUSH); // Limpa o eco do ATE0

    // 3. Enviar o comando real
    write(fd, "AT+CSQ\r\n", 8);

    // 4. Ler até 511 bytes (o timeout de 1s ajuda aqui)
    memset(buffer, 0, sizeof(buffer));
    int total_lido = 0;
    int n;
    
    // Pequeno loop para garantir que pegamos a resposta que demora a chegar
    for(int i=0; i<5; i++) {
        n = read(fd, buffer + total_lido, sizeof(buffer) - total_lido - 1);
        if (n > 0) total_lido += n;
        if (strstr(buffer, "OK") || strstr(buffer, "ERROR")) break;
        usleep(200000); // Espera 200ms entre tentativas
    }

    // 5. Processar resultado
    char *p = strstr(buffer, "+CSQ:");
    if (p) {
        int rssi, ber;
        if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) == 2) {
            printf("{\"status\": \"Success\", \"data\": {\"rssi\": %d}}\n", rssi);
        } else {
            printf("{\"status\": \"Error\", \"msg\": \"Falha ao extrair valores RSSI\"}\n");
        }
    } else {
        printf("{\"status\": \"Error\", \"msg\": \"Sem resposta valida do modem\"}\n");
    }

    close(fd);
    return 0;
}