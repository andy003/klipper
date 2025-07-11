// Example code for interacting with serial_irq.c via TCP (Multi-threaded - Simple)
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
static pthread_t tcp_thread;
static volatile int tcp_thread_running = 0;
static volatile int shutdown_requested = 0;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

static void* tcp_thread_func(void* arg)
{
    (void)arg; // Unused parameter
    struct sockaddr_in address;
    int opt = 1;
    
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    // Create and setup server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        tcp_thread_running = 0;
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
        tcp_thread_running = 0;
        return NULL;
    }
    
    tcp_thread_running = 1;
    
    // Main TCP loop
    while (!shutdown_requested) {
        // Accept new connection
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        if (select(server_fd + 1, &read_fds, NULL, NULL, &timeout) <= 0) {
            continue;
        }
        
        int new_client = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (new_client < 0) {
            continue;
        }
        
        pthread_mutex_lock(&client_mutex);
        if (client_fd != -1) {
            close(client_fd);
        }
        client_fd = new_client;
        pthread_mutex_unlock(&client_mutex);
        
        // Handle client communication
        uint8_t buffer[256];
        while (!shutdown_requested) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(client_fd, &fds);
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; // 100ms
            
            int activity = select(client_fd + 1, &fds, NULL, NULL, &timeout);
            
            if (activity < 0) {
                break; // Error
            }
            
            // Handle incoming data
            if (activity > 0 && FD_ISSET(client_fd, &fds)) {
                ssize_t bytes = read(client_fd, buffer, sizeof(buffer));
                if (bytes <= 0) {
                    break; // Client disconnected
                }
                
                // Process received data directly
                for (ssize_t i = 0; i < bytes; i++) {
                    serial_rx_byte(buffer[i]);
                }
            }
            
            // Handle outgoing data
            uint8_t tx_data;
            while (serial_get_tx_byte(&tx_data) == 0) {
                if (write(client_fd, &tx_data, 1) <= 0) {
                    goto client_disconnected;
                }
            }
        }
        
        client_disconnected:
        pthread_mutex_lock(&client_mutex);
        close(client_fd);
        client_fd = -1;
        pthread_mutex_unlock(&client_mutex);
    }
    
    // Cleanup
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    pthread_mutex_lock(&client_mutex);
    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }
    pthread_mutex_unlock(&client_mutex);
    
    tcp_thread_running = 0;
    return NULL;
}

void
serial_init(void)
{
    shutdown_requested = 0;
    pthread_create(&tcp_thread, NULL, tcp_thread_func, NULL);
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
    // In this simple version, the TCP thread handles all TX data directly
    // This function is called by the main thread but TX is handled in TCP thread
}

void
serial_cleanup(void)
{
    shutdown_requested = 1;
    if (tcp_thread_running) {
        pthread_join(tcp_thread, NULL);
    }
}
