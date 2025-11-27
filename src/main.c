#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "error.h"

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_PENDING_WRITES 8192

// Write buffer for handling non-blocking writes
typedef struct {
    char data[MAX_PENDING_WRITES];
    size_t size;   // Total data in buffer
    size_t offset; // How much we've already sent
} WriteBuffer;

// Client state
typedef struct {
    int fd;
    WriteBuffer write_buf;
} Client;

// Set a file descriptor to non-blocking mode
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL)");
        return -1;
    }
    return 0;
}

// Initialize write buffer
void init_write_buffer(WriteBuffer *buf) {
    buf->size = 0;
    buf->offset = 0;
}

// Check if write buffer is empty
bool write_buffer_empty(WriteBuffer *buf) { return buf->offset >= buf->size; }

// Add data to write buffer
bool write_buffer_append(WriteBuffer *buf, const char *data, size_t len) {
    // Check if we have space
    if (buf->size + len > MAX_PENDING_WRITES) {
        fprintf(stderr, "Write buffer full, cannot append %zu bytes\n", len);
        return false;
    }

    // If buffer was consumed, reset it
    if (buf->offset >= buf->size) {
        buf->size = 0;
        buf->offset = 0;
    }

    // Append data
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
    return true;
}

// Try to send data from write buffer
// Returns: 0 on success (all sent), -1 on error, 1 if more data remains
int write_buffer_flush(WriteBuffer *buf, int fd) {
    while (buf->offset < buf->size) {
        ssize_t sent = send(fd, buf->data + buf->offset, buf->size - buf->offset, 0);

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, try again later
                return 1; // More data remains
            }
            // Real error
            perror("send");
            return -1;
        }

        buf->offset += sent;
    }

    // All data sent, reset buffer
    buf->size = 0;
    buf->offset = 0;
    return 0;
}

// Helper function to close and clean up a client connection
void close_client(Client *clients, int list_index, fd_set *master_read_set, fd_set *master_write_set) {
    int fd = clients[list_index].fd;
    if (fd < 0) {
        return;
    }

    printf("Closing client fd=%d\n", fd);
    close(fd);
    FD_CLR(fd, master_read_set);
    FD_CLR(fd, master_write_set);
    clients[list_index].fd = -1;
    init_write_buffer(&clients[list_index].write_buf);
}

// Create and configure server socket
int create_server_hello_socket(int port) {
    int server_fd;
    struct sockaddr_in server_addr;

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        fatal_error("Socket creation failed");
    }

    // Set non-blocking
    if (set_nonblocking(server_fd) < 0) {
        close(server_fd);
        fatal_error("Failed to set server socket non-blocking");
    }

    // Set SO_REUSEADDR to allow quick restart
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(server_fd);
        fatal_error("Failed to set SO_REUSEADDR");
    }

    // Bind
    server_addr = (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_addr = {.s_addr = htonl(INADDR_ANY)},
        .sin_port = htons(port),
    };

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        fatal_error("Bind failed");
    }

    // Listen
    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(server_fd);
        fatal_error("Listen failed");
    }

    printf("Server listening on port %d\n", port);
    return server_fd;
}

// Handle new incoming connection
void handle_new_connection(int server_fd, Client *clients, fd_set *master_read_set, int *max_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_size = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_size);
    if (client_fd == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("accept");
        }
        return;
    }

    // Set client socket to non-blocking
    if (set_nonblocking(client_fd) < 0) {
        close(client_fd);
        return;
    }

    // Add to master read set
    FD_SET(client_fd, master_read_set);

    // Update max_fd
    if (client_fd > *max_fd) {
        *max_fd = client_fd;
    }

    // Find empty slot in client list
    bool added = false;
    for (int i = 0; i < FD_SETSIZE; ++i) {
        if (clients[i].fd < 0) {
            clients[i].fd = client_fd;
            init_write_buffer(&clients[i].write_buf);
            added = true;
            break;
        }
    }

    if (!added) {
        fprintf(stderr, "Too many clients, rejecting connection\n");
        close(client_fd);
        FD_CLR(client_fd, master_read_set);
        return;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    printf("New client connected: %s:%d (fd=%d)\n", client_ip, ntohs(client_addr.sin_port), client_fd);
}

// Handle client data (echo server)
void handle_client_read(Client *clients, int list_index, fd_set *master_read_set, fd_set *master_write_set) {
    int fd = clients[list_index].fd;
    char buffer[BUFFER_SIZE];

    // Read data from client
    ssize_t bytes_received = recv(fd, buffer, sizeof(buffer), 0);

    if (bytes_received < 0) {
        // Error during recv
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available right now (shouldn't happen with select, but safe to check)
            return;
        }
        // Real error
        perror("recv");
        close_client(clients, list_index, master_read_set, master_write_set);
        return;
    }

    if (bytes_received == 0) {
        // Client closed connection
        printf("Client disconnected (fd=%d)\n", fd);
        close_client(clients, list_index, master_read_set, master_write_set);
        return;
    }

    printf("Received %zd bytes from client (fd=%d)\n", bytes_received, fd);

    // Add data to write buffer
    if (!write_buffer_append(&clients[list_index].write_buf, buffer, bytes_received)) {
        fprintf(stderr, "Write buffer overflow for fd=%d, closing connection\n", fd);
        close_client(clients, list_index, master_read_set, master_write_set);
        return;
    }

    // Add to write set since we have data to send
    FD_SET(fd, master_write_set);
}

// Handle client write (flush write buffer)
void handle_client_write(Client *clients, int list_index, fd_set *master_read_set, fd_set *master_write_set) {
    int fd = clients[list_index].fd;
    WriteBuffer *buf = &clients[list_index].write_buf;

    int result = write_buffer_flush(buf, fd);

    if (result == -1) {
        // Error occurred
        close_client(clients, list_index, master_read_set, master_write_set);
        return;
    }

    if (result == 0) {
        // All data sent, remove from write set
        FD_CLR(fd, master_write_set);
        printf("Finished sending data to client (fd=%d)\n", fd);
    }
    // If result == 1, more data remains, keep in write set
}

// Main server loop using select()
int run_server_with_select(int server_fd) {
    // Why do we need master sets
    //   After select returns:
    //      read_set now ONLY contains the fds that are ready!

    fd_set master_read_set, master_write_set; // PERSISTENT - never modified by select()
    fd_set read_set, write_set;               // WORKING COPIES - modified by select()

    int max_fd = server_fd;

    // Initialize master sets
    FD_ZERO(&master_read_set);
    FD_ZERO(&master_write_set);

    FD_SET(server_fd, &master_read_set);

    // Track all client connections
    Client clients[FD_SETSIZE];
    for (int i = 0; i < FD_SETSIZE; ++i) {
        clients[i].fd = -1;
        init_write_buffer(&clients[i].write_buf);
    }

    printf("Server ready, waiting for connections...\n");

    // Main event loop
    while (true) {
        // Copy master sets (select modifies them)
        read_set = master_read_set;
        write_set = master_write_set;

        // Wait for activity on any socket
        int activity = select(max_fd + 1, &read_set, &write_set, NULL, NULL);

        if (activity < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, continue
                continue;
            }
            perror("select");
            return -1;
        }

        // Check if server socket has a new connection
        if (FD_ISSET(server_fd, &read_set)) {
            handle_new_connection(server_fd, clients, &master_read_set, &max_fd);
        }

        // Check all client sockets for activity
        for (int i = 0; i < FD_SETSIZE; i++) {
            int fd = clients[i].fd;

            // Skip empty slots
            if (fd < 0) {
                continue;
            }

            // Check if this client is ready for reading
            if (FD_ISSET(fd, &read_set)) {
                handle_client_read(clients, i, &master_read_set, &master_write_set);
            }

            // Check if this client is ready for writing
            // Only check if fd is still valid (might have been closed in read handler)
            if (clients[i].fd >= 0 && FD_ISSET(fd, &write_set)) {
                handle_client_write(clients, i, &master_read_set, &master_write_set);
            }
        }
    }

    return 0;
}

int main(void) {
    int server_fd = create_server_hello_socket(PORT);
    int result = run_server_with_select(server_fd);
    close(server_fd);
    return result;
}
