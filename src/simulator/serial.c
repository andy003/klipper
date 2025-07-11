// Example code for interacting with serial_irq.c via TCP (Multi-threaded)
//
// Copyright (C) 2018  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <fcntl.h> // fcntl
#include <unistd.h> // close, pipe
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
#define MAX_CLIENTS 1

// Global variables
static int server_fd = -1;
static int client_fd = -1;
static pthread_t tcp_thread;
static volatile int tcp_thread_running = 0;
static volatile int shutdown_requested = 0;

// Pipes for communication between threads
static int rx_pipe[2] = {-1, -1}; // From TCP thread to main thread
static int tx_pipe[2] = {-1, -1}; // From main thread to TCP thread

// Mutex for client_fd access
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

static void tcp_server_cleanup(void)
{
    pthread_mutex_lock(&client_mutex);
    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }
    pthread_mutex_unlock(&client_mutex);
    
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
}

static int tcp_server_init(void)
{
    struct sockaddr_in address;
    int opt = 1;
    
    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        return -1;
    }
    
    // Allow socket reuse
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt))) {
        tcp_server_cleanup();
        return -1;
    }
    
    // Configure address
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_PORT);
    
    // Bind socket to address
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        tcp_server_cleanup();
        return -1;
    }
    
    // Listen for connections
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        tcp_server_cleanup();
        return -1;
    }
    
    return 0;
}

static void handle_client_connection(int sock)
{
    fd_set read_fds, write_fds;
    struct timeval timeout;
    uint8_t buffer[256];
    uint8_t tx_data;
    
    while (!shutdown_requested) {
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(sock, &read_fds);
        FD_SET(tx_pipe[0], &read_fds); // Check for data to send
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout
        
        int max_fd = (sock > tx_pipe[0]) ? sock : tx_pipe[0];
        int activity = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            break; // Error in select
        }
        
        // Handle incoming data from client
        if (FD_ISSET(sock, &read_fds)) {
            ssize_t bytes_read = read(sock, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                break; // Client disconnected or error
            }
            
            // Forward received data to main thread via pipe
            for (ssize_t i = 0; i < bytes_read; i++) {
                write(rx_pipe[1], &buffer[i], 1);
            }
        }
        
        // Handle outgoing data from main thread
        if (FD_ISSET(tx_pipe[0], &read_fds)) {
            if (read(tx_pipe[0], &tx_data, 1) == 1) {
                if (write(sock, &tx_data, 1) <= 0) {
                    break; // Error sending data
                }
            }
        }
    }
}

static void* tcp_thread_func(void* arg)
{
    (void)arg; // Unused parameter
    
    // Ignore SIGPIPE to handle broken connections gracefully
    signal(SIGPIPE, SIG_IGN);
    
    if (tcp_server_init() < 0) {
        tcp_thread_running = 0;
        return NULL;
    }
    
    tcp_thread_running = 1;
    
    while (!shutdown_requested) {
        // Wait for new connection
        struct sockaddr_in address;
        socklen_t addrlen = sizeof(address);
        
        fd_set read_fds;
        struct timeval timeout;
        
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        int activity = select(server_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity > 0 && FD_ISSET(server_fd, &read_fds)) {
            int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
            if (new_socket >= 0) {
                pthread_mutex_lock(&client_mutex);
                if (client_fd != -1) {
                    close(client_fd); // Close existing client
                }
                client_fd = new_socket;
                pthread_mutex_unlock(&client_mutex);
                
                // Handle this connection
                handle_client_connection(new_socket);
                
                // Client disconnected
                pthread_mutex_lock(&client_mutex);
                close(client_fd);
                client_fd = -1;
                pthread_mutex_unlock(&client_mutex);
            }
        }
    }
    
    tcp_server_cleanup();
    tcp_thread_running = 0;
    return NULL;
}

void
serial_init(void)
{
    // Create pipes for communication between threads
    if (pipe(rx_pipe) == -1 || pipe(tx_pipe) == -1) {
        return; // Failed to create pipes
    }
    
    // Make pipes non-blocking
    fcntl(rx_pipe[0], F_SETFL, fcntl(rx_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(tx_pipe[1], F_SETFL, fcntl(tx_pipe[1], F_GETFL, 0) | O_NONBLOCK);
    
    // Start TCP thread
    shutdown_requested = 0;
    if (pthread_create(&tcp_thread, NULL, tcp_thread_func, NULL) == 0) {
        // Wait a bit for thread to initialize
        usleep(100000); // 100ms
    }
}
DECL_INIT(serial_init);

void *
console_receive_buffer(void)
{
    return NULL;
}

// Called from main thread to process received data
static void
process_received_data(void)
{
    uint8_t data;
    while (read(rx_pipe[0], &data, 1) == 1) {
        serial_rx_byte(data);
    }
}

void
serial_enable_tx_irq(void)
{
    // Process any received data first
    process_received_data();
    
    // Send pending TX data to TCP thread
    if (tcp_thread_running) {
        uint8_t data;
        while (serial_get_tx_byte(&data) == 0) {
            if (write(tx_pipe[1], &data, 1) <= 0) {
                break; // Pipe full or error
            }
        }
    }
}

// Cleanup function (should be called on program exit)
void
serial_cleanup(void)
{
    if (tcp_thread_running) {
        shutdown_requested = 1;
        
        // Wait for TCP thread to finish
        pthread_join(tcp_thread, NULL);
    }
    
    // Close pipes
    if (rx_pipe[0] != -1) {
        close(rx_pipe[0]);
        close(rx_pipe[1]);
        rx_pipe[0] = rx_pipe[1] = -1;
    }
    
    if (tx_pipe[0] != -1) {
        close(tx_pipe[0]);
        close(tx_pipe[1]);
        tx_pipe[0] = tx_pipe[1] = -1;
    }
}
