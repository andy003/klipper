// Example code for interacting with serial_irq.c via TCP
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
#include "board/serial_irq.h" // serial_get_tx_byte
#include "sched.h" // DECL_INIT

#define TCP_PORT 8080
#define MAX_CLIENTS 1

static int server_fd = -1;
static int client_fd = -1;

void
serial_init(void)
{
    struct sockaddr_in address;
    int opt = 1;
    
    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        return; // Failed to create socket
    }
    
    // Allow socket reuse
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt))) {
        close(server_fd);
        server_fd = -1;
        return;
    }
    
    // Configure address
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_PORT);
    
    // Bind socket to address
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(server_fd);
        server_fd = -1;
        return;
    }
    
    // Listen for connections
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        close(server_fd);
        server_fd = -1;
        return;
    }
    
    // Make server socket non-blocking
    fcntl(server_fd, F_SETFL, fcntl(server_fd, F_GETFL, 0) | O_NONBLOCK);
}
DECL_INIT(serial_init);

void *
console_receive_buffer(void)
{
    return NULL;
}

static void
check_for_new_connections(void)
{
    if (server_fd == -1 || client_fd != -1)
        return; // No server or already have a client
        
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    int new_socket = accept(server_fd, (struct sockaddr *)&address, 
                           (socklen_t*)&addrlen);
    if (new_socket >= 0) {
        // Make client socket non-blocking
        fcntl(new_socket, F_SETFL, fcntl(new_socket, F_GETFL, 0) | O_NONBLOCK);
        client_fd = new_socket;
    }
}

static void
check_for_received_data(void)
{
    if (client_fd == -1)
        return;
        
    uint8_t buffer[256];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));
    
    if (bytes_read > 0) {
        // Process received data byte by byte
        for (ssize_t i = 0; i < bytes_read; i++) {
            serial_rx_byte(buffer[i]);
        }
    } else if (bytes_read == 0 || (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        // Client disconnected or error
        close(client_fd);
        client_fd = -1;
    }
}

static void
do_tcp_uart(void)
{
    // Check for new connections
    check_for_new_connections();
    
    // Check for received data
    check_for_received_data();
    
    // Send pending data
    if (client_fd != -1) {
        for (;;) {
            uint8_t data;
            int ret = serial_get_tx_byte(&data);
            if (ret)
                break;
                
            ssize_t bytes_sent = write(client_fd, &data, sizeof(data));
            if (bytes_sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // Client disconnected or error
                close(client_fd);
                client_fd = -1;
                break;
            }
        }
    }
}

void
serial_enable_tx_irq(void)
{
    // Handle TCP communication instead of hardware irq
    do_tcp_uart();
}
