/*
 * Simple eventfd example - thread signaling
 * Compile: gcc -o eventfd_demo eventfd_demo.c -pthread
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>

int efd;

void* worker_thread(void* arg) {
    printf("[Worker] Doing some work...\n");
    sleep(2);
    
    printf("[Worker] Done! Signaling main thread\n");
    uint64_t val = 1;
    write(efd, &val, sizeof(val));
    
    return NULL;
}

int main() {
    // Create eventfd with initial value 0
    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        return 1;
    }

    // Start worker thread
    pthread_t thread;
    pthread_create(&thread, NULL, worker_thread, NULL);

    // Main thread waits for signal
    printf("[Main] Waiting for worker...\n");
    uint64_t val;
    read(efd, &val, sizeof(val));  // Blocks until worker writes
    printf("[Main] Got signal! val=%lu\n", val);

    pthread_join(thread, NULL);
    close(efd);
    return 0;
}
