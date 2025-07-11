// Example code for interacting with serial_irq.c via TCP (Multi-threaded - Direct Send)
//
// Copyright (C) 2018  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <fcntl.h> // fcntl
#include <unistd.h> // close
#include <sys/socket.h> // socket, bind, listen, accept
#include <netinet/in.h> // sockaddr_in
#include <arpa/inet.h> // inet_addr
#include <errno.h> // errno
#include <string.h> // memset
#include <pthread.h> // pthread_create, pthread_mutex_t
#include <signal.h> // signal
#include "board/serial_irq.h" // serial_get_tx_byte
#include "sched.h" // DECL_INIT

#define TCP_PORT 8080

// Global variables
static int server_fd = -1;
static int client_fd = -1;
static pthread_t accept_thread;
static pthread_t read_thread;
static volatile int threads_running = 0;
static volatile int shutdown_requested = 0;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

// Accept thread - handles new connections
static void* accept_thread_func(void* arg)
{
    (void)arg;
    struct sockaddr_in address;
    int opt = 1;
    
    // Create and setup server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        return NULL;
    }
    
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server_fd, 1) < 0) {
        close(server_fd);
        server_fd = -1;
        return NULL;
    }
    
    threads_running = 1;
    
    // Accept connections loop
    while (!shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int new_client = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (new_client < 0) {
            if (!shutdown_requested) {
                usleep(100000); // Wait 100ms on error
            }
            continue;
        }
        
        pthread_mutex_lock(&client_mutex);
        if (client_fd != -1) {
            close(client_fd);
        }
        client_fd = new_client;
        pthread_mutex_unlock(&client_mutex);
    }
    
    // Cleanup
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    return NULL;
}

// Read thread - handles incoming data
static void* read_thread_func(void* arg)
{
    (void)arg;
    uint8_t buffer[256];
    
    while (!shutdown_requested) {
        pthread_mutex_lock(&client_mutex);
        int current_client = client_fd;
        pthread_mutex_unlock(&client_mutex);
        
        if (current_client == -1) {
            usleep(100000); // Wait 100ms if no client
            continue;
        }
        
        ssize_t bytes = read(current_client, buffer, sizeof(buffer));
        if (bytes > 0) {
            // Process received data
            for (ssize_t i = 0; i < bytes; i++) {
                serial_rx_byte(buffer[i]);
            }
        } else {
            // Client disconnected or error
            pthread_mutex_lock(&client_mutex);
            if (client_fd == current_client) {
                close(client_fd);
                client_fd = -1;
            }
            pthread_mutex_unlock(&client_mutex);
        }
    }
    return NULL;
}

void
serial_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    shutdown_requested = 0;
    client_fd = -1;
    
    // Start threads
    pthread_create(&accept_thread, NULL, accept_thread_func, NULL);
    pthread_create(&read_thread, NULL, read_thread_func, NULL);
}
DECL_INIT(serial_init);

void *
console_receive_buffer(void)
{
    return NULL;
}

void
serial_enable_tx_irq(void)
{
    // Send data directly in main thread
    pthread_mutex_lock(&client_mutex);
    int current_client = client_fd;
    pthread_mutex_unlock(&client_mutex);
    
    if (current_client != -1) {
        uint8_t data;
        while (serial_get_tx_byte(&data) == 0) {
            if (write(current_client, &data, 1) <= 0) {
                // Client disconnected or error
                pthread_mutex_lock(&client_mutex);
                if (client_fd == current_client) {
                    close(client_fd);
                    client_fd = -1;
                }
                pthread_mutex_unlock(&client_mutex);
                break;
            }
        }
    }
}

void
serial_cleanup(void)
{
    shutdown_requested = 1;
    
    // Close server socket to unblock accept
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    
    // Close client socket
    pthread_mutex_lock(&client_mutex);
    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }
    pthread_mutex_unlock(&client_mutex);
    
    // Wait for threads to finish
    if (threads_running) {
        pthread_join(accept_thread, NULL);
        pthread_join(read_thread, NULL);
    }
}
