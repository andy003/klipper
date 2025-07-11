// Example code for interacting with serial_irq.c via TCP (Multi-threaded)
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
#define MAX_CLIENTS 1
#define BUFFER_SIZE 1024

// Ring buffer structure for thread-safe data transfer
typedef struct {
    uint8_t data[BUFFER_SIZE];
    volatile int head;
    volatile int tail;
    volatile int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond_not_empty;
    pthread_cond_t cond_not_full;
} ring_buffer_t;

// Global variables
static int server_fd = -1;
static int client_fd = -1;
static pthread_t tcp_thread;
static volatile int tcp_thread_running = 0;
static volatile int shutdown_requested = 0;

// Ring buffers for data transfer between threads
static ring_buffer_t tx_buffer; // Data from main thread to TCP thread
static ring_buffer_t rx_buffer; // Data from TCP thread to main thread

// Ring buffer functions
static void ring_buffer_init(ring_buffer_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    pthread_mutex_init(&rb->mutex, NULL);
    pthread_cond_init(&rb->cond_not_empty, NULL);
    pthread_cond_init(&rb->cond_not_full, NULL);
}

static void ring_buffer_destroy(ring_buffer_t *rb)
{
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->cond_not_empty);
    pthread_cond_destroy(&rb->cond_not_full);
}

static int ring_buffer_put(ring_buffer_t *rb, uint8_t data)
{
    pthread_mutex_lock(&rb->mutex);
    
    while (rb->count >= BUFFER_SIZE && !shutdown_requested) {
        pthread_cond_wait(&rb->cond_not_full, &rb->mutex);
    }
    
    if (shutdown_requested) {
        pthread_mutex_unlock(&rb->mutex);
        return -1;
    }
    
    rb->data[rb->head] = data;
    rb->head = (rb->head + 1) % BUFFER_SIZE;
    rb->count++;
    
    pthread_cond_signal(&rb->cond_not_empty);
    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

static int ring_buffer_get(ring_buffer_t *rb, uint8_t *data)
{
    pthread_mutex_lock(&rb->mutex);
    
    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->mutex);
        return -1; // No data available
    }
    
    *data = rb->data[rb->tail];
    rb->tail = (rb->tail + 1) % BUFFER_SIZE;
    rb->count--;
    
    pthread_cond_signal(&rb->cond_not_full);
    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

static int ring_buffer_get_blocking(ring_buffer_t *rb, uint8_t *data)
{
    pthread_mutex_lock(&rb->mutex);
    
    while (rb->count == 0 && !shutdown_requested) {
        pthread_cond_wait(&rb->cond_not_empty, &rb->mutex);
    }
    
    if (shutdown_requested) {
        pthread_mutex_unlock(&rb->mutex);
        return -1;
    }
    
    *data = rb->data[rb->tail];
    rb->tail = (rb->tail + 1) % BUFFER_SIZE;
    rb->count--;
    
    pthread_cond_signal(&rb->cond_not_full);
    pthread_mutex_unlock(&rb->mutex);
    return 0;
}

static void tcp_server_cleanup(void)
{
    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }
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
    
    while (!shutdown_requested && client_fd == sock) {
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(sock, &read_fds);
        
        // Check if we have data to send
        pthread_mutex_lock(&tx_buffer.mutex);
        int has_tx_data = (tx_buffer.count > 0);
        pthread_mutex_unlock(&tx_buffer.mutex);
        
        if (has_tx_data) {
            FD_SET(sock, &write_fds);
        }
        
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms timeout
        
        int activity = select(sock + 1, &read_fds, &write_fds, NULL, &timeout);
        
        if (activity < 0 && errno != EINTR) {
            break; // Error in select
        }
        
        // Handle incoming data
        if (FD_ISSET(sock, &read_fds)) {
            ssize_t bytes_read = read(sock, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                break; // Client disconnected or error
            }
            
            // Put received data into RX buffer
            for (ssize_t i = 0; i < bytes_read; i++) {
                ring_buffer_put(&rx_buffer, buffer[i]);
            }
        }
        
        // Handle outgoing data
        if (FD_ISSET(sock, &write_fds)) {
            uint8_t data;
            if (ring_buffer_get(&tx_buffer, &data) == 0) {
                ssize_t bytes_sent = write(sock, &data, 1);
                if (bytes_sent <= 0) {
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
        if (client_fd == -1) {
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
                    client_fd = new_socket;
                }
            }
        } else {
            // Handle existing connection
            handle_client_connection(client_fd);
            
            // Client disconnected
            close(client_fd);
            client_fd = -1;
        }
    }
    
    tcp_server_cleanup();
    tcp_thread_running = 0;
    return NULL;
}

void
serial_init(void)
{
    // Initialize ring buffers
    ring_buffer_init(&tx_buffer);
    ring_buffer_init(&rx_buffer);
    
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
    while (ring_buffer_get(&rx_buffer, &data) == 0) {
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
            if (ring_buffer_put(&tx_buffer, data) < 0) {
                break; // Buffer full or shutdown requested
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
        
        // Wake up any waiting threads
        pthread_cond_broadcast(&tx_buffer.cond_not_empty);
        pthread_cond_broadcast(&tx_buffer.cond_not_full);
        pthread_cond_broadcast(&rx_buffer.cond_not_empty);
        pthread_cond_broadcast(&rx_buffer.cond_not_full);
        
        // Wait for TCP thread to finish
        pthread_join(tcp_thread, NULL);
    }
    
    // Cleanup ring buffers
    ring_buffer_destroy(&tx_buffer);
    ring_buffer_destroy(&rx_buffer);
}
