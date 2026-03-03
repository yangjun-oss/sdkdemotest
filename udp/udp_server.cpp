#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>

constexpr int DEFAULT_PORT = 8888;
constexpr int BUF_SIZE = 1024;

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Error: failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(sockfd, reinterpret_cast<sockaddr *>(&server_addr),
             sizeof(server_addr)) < 0) {
        std::cerr << "Error: bind failed" << std::endl;
        close(sockfd);
        return 1;
    }

    std::cout << "UDP server listening on port " << port << std::endl;

    char buf[BUF_SIZE];
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        ssize_t n = recvfrom(sockfd, buf, BUF_SIZE - 1, 0,
                             reinterpret_cast<sockaddr *>(&client_addr),
                             &client_len);
        if (n < 0) {
            std::cerr << "Error: recvfrom failed" << std::endl;
            break;
        }

        buf[n] = '\0';
        std::cout << "Received from " << inet_ntoa(client_addr.sin_addr)
                  << ":" << ntohs(client_addr.sin_port)
                  << " -> " << buf << std::endl;

        // Echo the message back to the client
        if (sendto(sockfd, buf, static_cast<size_t>(n), 0,
                   reinterpret_cast<sockaddr *>(&client_addr),
                   client_len) < 0) {
            std::cerr << "Error: sendto failed" << std::endl;
        }
    }

    close(sockfd);
    return 0;
}
