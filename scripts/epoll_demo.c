/*
 * epoll + eventfd + socket example
 * One event loop handles both network and custom events
 *
 * Compile: gcc -o epoll_demo epoll_demo.c -pthread
 * Run: ./epoll_demo
 * Test: nc localhost 8080
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

int efd;

void* signal_thread(void* arg) {
    // Periodically signal the event loop
    for (int i = 1; i <= 3; i++) {
        sleep(3);
        printf("[Signal] Sending event %d\n", i);
        uint64_t val = i;
        write(efd, &val, sizeof(val));
    }
    return NULL;
}

int main() {
    // Create eventfd
    efd = eventfd(0, 0);

    // Create TCP server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(8080),
        .sin_addr.s_addr = INADDR_ANY
    };
    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);
    printf("Server listening on port 8080\n");

    // Create epoll
    int epoll_fd = epoll_create1(0);

    // Add eventfd to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = efd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, efd, &ev);

    // Add server socket to epoll
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

    // Start signal thread
    pthread_t thread;
    pthread_create(&thread, NULL, signal_thread, NULL);

    // Event loop
    printf("Event loop started (Ctrl+C to exit)\n");
    printf("Try: nc localhost 8080\n\n");

    struct epoll_event events[10];
    while (1) {
        int n = epoll_wait(epoll_fd, events, 10, -1);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == efd) {
                // Custom event from eventfd
                uint64_t val;
                read(efd, &val, sizeof(val));
                printf("[Loop] Got custom event: %lu\n", val);

            } else if (fd == server_fd) {
                // New connection
                int client = accept(server_fd, NULL, NULL);
                printf("[Loop] New connection: fd=%d\n", client);

                // Add client to epoll
                ev.events = EPOLLIN;
                ev.data.fd = client;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev);

            } else {
                // Data from client
                char buf[256];
                int len = read(fd, buf, sizeof(buf) - 1);
                if (len <= 0) {
                    printf("[Loop] Client %d disconnected\n", fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else {
                    buf[len] = 0;
                    printf("[Loop] Client %d: %s", fd, buf);
                    write(fd, "OK\n", 3);
                }
            }
        }
    }

    return 0;
}
