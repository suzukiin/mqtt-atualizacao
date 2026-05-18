#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int is_online() {
    int sockfd;
    struct sockaddr_in serv_addr;
    
    // Cria o socket (AF_INET = IPv4, SOCK_STREAM = TCP)
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return 0;

    // Configura o endereço do servidor (Google DNS: 8.8.8.8)
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(53); // Porta DNS
    serv_addr.sin_addr.s_addr = inet_addr("8.8.8.8");

    // Define um timeout para a tentativa de conexão (2 segundos)
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Tenta conectar
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        return 0; // Offline
    }

    close(sockfd);
    return 1; // Online
}

int main() {
    printf("Content-Type: application/json\r\n\r\n");
    
    if (is_online()) {
        printf("{\"status\": \"Success\", \"internet\": \"online\"}\n");
    } else {
        printf("{\"status\": \"Error\", \"internet\": \"offline\"}\n");
    }
    
    return 0;
}