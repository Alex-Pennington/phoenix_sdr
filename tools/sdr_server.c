/**
 * @file sdr_server.c
 * @brief Phoenix SDR TCP Control Server
 *
 * TCP server for remote control of SDRplay RSP2 Pro.
 * Implements protocol defined in docs/SDR_TCP_CONTROL_INTERFACE.md
 *
 * Usage: sdr_server.exe [-p port] [-T addr]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

#include "tcp_server.h"
#include "version.h"

/*============================================================================
 * Globals
 *============================================================================*/

static volatile bool g_running = true;
static SOCKET g_listen_socket = INVALID_SOCKET;
static SOCKET g_client_socket = INVALID_SOCKET;

/*============================================================================
 * Signal Handler
 *============================================================================*/

static void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    g_running = false;
    
    /* Close sockets to unblock accept/recv */
    if (g_client_socket != INVALID_SOCKET) {
        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
    }
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
        g_listen_socket = INVALID_SOCKET;
    }
}

/*============================================================================
 * Socket Helpers
 *============================================================================*/

static int socket_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    return 0;
#endif
}

static void socket_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static int send_line(SOCKET sock, const char *line) {
    size_t len = strlen(line);
    int sent = send(sock, line, (int)len, 0);
    return (sent == (int)len) ? 0 : -1;
}

static int recv_line(SOCKET sock, char *buf, int buf_size) {
    int total = 0;
    while (total < buf_size - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            /* Connection closed or error */
            return -1;
        }
        if (c == '\n') {
            buf[total] = '\0';
            return total;
        }
        if (c != '\r') {
            buf[total++] = c;
        }
    }
    buf[total] = '\0';
    return total;
}

/*============================================================================
 * Client Handler
 *============================================================================*/

static void handle_client(SOCKET client, tcp_sdr_state_t *state) {
    char line[TCP_MAX_LINE_LENGTH];
    char response[TCP_MAX_LINE_LENGTH + 16];
    tcp_command_t cmd;
    tcp_response_t resp;
    
    printf("Client connected\n");
    
    while (g_running) {
        /* Receive command */
        int len = recv_line(client, line, sizeof(line));
        if (len < 0) {
            printf("Client disconnected\n");
            break;
        }
        if (len == 0) {
            continue;  /* Empty line */
        }
        
        printf("< %s\n", line);
        
        /* Parse command */
        tcp_error_t err = tcp_parse_command(line, &cmd);
        
        if (err == TCP_OK) {
            /* Execute command */
            tcp_execute_command(&cmd, state, &resp);
        } else {
            /* Parse error */
            const char *msg = NULL;
            switch (err) {
                case TCP_ERR_SYNTAX:   msg = "malformed command"; break;
                case TCP_ERR_UNKNOWN:  msg = "unknown command"; break;
                case TCP_ERR_RANGE:    msg = "value out of range"; break;
                case TCP_ERR_PARAM:    msg = "invalid parameter"; break;
                default:               msg = NULL; break;
            }
            tcp_response_error(&resp, err, msg);
        }
        
        /* Format and send response */
        tcp_format_response(&resp, response, sizeof(response));
        printf("> %s", response);  /* Already has \n */
        
        if (send_line(client, response) < 0) {
            printf("Send failed\n");
            break;
        }
        
        /* Handle QUIT */
        if (cmd.type == CMD_QUIT) {
            break;
        }
    }
    
    /* Stop streaming if client disconnects */
    if (state->streaming) {
        printf("Stopping streaming (client disconnect)\n");
        state->streaming = false;
        /* TODO: Actually stop SDR */
    }
}

/*============================================================================
 * Usage
 *============================================================================*/

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -p PORT    Listen port (default: %d)\n", TCP_DEFAULT_PORT);
    printf("  -T ADDR    Listen address (default: 127.0.0.1)\n");
    printf("  -h         Show this help\n");
    printf("\nProtocol: See docs/SDR_TCP_CONTROL_INTERFACE.md\n");
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    print_version("Phoenix SDR - TCP Control Server");
    
    int port = TCP_DEFAULT_PORT;
    const char *bind_addr = "127.0.0.1";
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    /* Initialize sockets */
    if (socket_init() != 0) {
        fprintf(stderr, "Socket initialization failed\n");
        return 1;
    }
    
    /* Set up signal handler */
    signal(SIGINT, signal_handler);
#ifndef _WIN32
    signal(SIGTERM, signal_handler);
#endif
    
    /* Create listen socket */
    g_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "Failed to create socket\n");
        socket_cleanup();
        return 1;
    }
    
    /* Allow address reuse */
    int optval = 1;
    setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));
    
    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
        closesocket(g_listen_socket);
        socket_cleanup();
        return 1;
    }
    
    if (bind(g_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed on %s:%d\n", bind_addr, port);
        closesocket(g_listen_socket);
        socket_cleanup();
        return 1;
    }
    
    /* Listen */
    if (listen(g_listen_socket, 1) == SOCKET_ERROR) {
        fprintf(stderr, "Listen failed\n");
        closesocket(g_listen_socket);
        socket_cleanup();
        return 1;
    }
    
    printf("Listening on %s:%d\n", bind_addr, port);
    printf("Press Ctrl+C to stop\n\n");
    
    /* Initialize SDR state */
    tcp_sdr_state_t state;
    tcp_state_defaults(&state);
    
    /* Accept loop */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        g_client_socket = accept(g_listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (g_client_socket == INVALID_SOCKET) {
            if (g_running) {
                fprintf(stderr, "Accept failed\n");
            }
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("Connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));
        
        /* Handle this client (blocking - single client only) */
        handle_client(g_client_socket, &state);
        
        closesocket(g_client_socket);
        g_client_socket = INVALID_SOCKET;
        printf("Client session ended\n\n");
    }
    
    /* Cleanup */
    if (g_listen_socket != INVALID_SOCKET) {
        closesocket(g_listen_socket);
    }
    socket_cleanup();
    
    printf("Server stopped\n");
    return 0;
}
