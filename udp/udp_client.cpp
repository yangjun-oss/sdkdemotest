#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>

constexpr int DEFAULT_PORT = 8888;
constexpr int BUF_SIZE = 1024;

int main(int argc, char *argv[]) {
    const char *server_ip = "127.0.0.1";
    int port = DEFAULT_PORT;

    if (argc > 1) {
        server_ip = argv[1];
    }
    if (argc > 2) {
        port = std::atoi(argv[2]);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Error: failed to create socket" << std::endl;
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Error: invalid server address" << std::endl;
        close(sockfd);
        return 1;
    }

    std::cout << "UDP client ready. Type a message and press Enter to send."
              << std::endl;
    std::cout << "Server: " << server_ip << ":" << port << std::endl;

    // Set a 2-second receive timeout so the client does not block forever
    timeval tv{2, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[BUF_SIZE];
    std::string line;

    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        if (sendto(sockfd, line.c_str(), line.size(), 0,
                   reinterpret_cast<sockaddr *>(&server_addr),
                   sizeof(server_addr)) < 0) {
            std::cerr << "Error: sendto failed" << std::endl;
            continue;
        }

        sockaddr_in from_addr{};
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(sockfd, buf, BUF_SIZE - 1, 0,
                             reinterpret_cast<sockaddr *>(&from_addr),
                             &from_len);
        if (n < 0) {
            std::cerr << "Error: no response from server (timeout)" << std::endl;
            continue;
        }

        buf[n] = '\0';
        std::cout << "Echo from server: " << buf << std::endl;
    }

    close(sockfd);
    return 0;
}
