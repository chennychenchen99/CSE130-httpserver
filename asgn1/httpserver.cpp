#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <arpa/inet.h>
#include <ctype.h>

#define SIZE 4096

using namespace std;

struct header {
    char* command;
    char* resource_name;
    int content_length;
};

struct header parseHeader(char buf[]) {
    struct header head;
    
    char* token = strtok(buf, " ");
    head.command = token;
    
    token = strtok(NULL, " ");
    head.resource_name = token;
    
    head.content_length = -1;
    
    while ((token = strtok(NULL, " ")) != NULL) {
        if (strstr(token, "\r\n\r\n") != NULL) {
            // marks end of header
            break;
        }
        
        if (strstr(token, "Content-Length") != NULL) {
            token = strtok(NULL, " ");
            char *p = token;
            long len;
            while (isdigit(*p) && *p != '\\') {
                len = strtol(p, &p, 10);
            }
            head.content_length = len;
        }
    }
    
    return head;
}

int send_response(int comm_fd, int response_num, int content_len, char* resource_name) {
    char response_1[200];
    const char* response_2;
    
    if (response_num == 400) {
        int k = snprintf(response_1, 200, "HTTP/1.1 400 Bad Request\r\nContent-Length: %d\r\n\r\n", content_len);
        response_1[k] = '\0';
        send(comm_fd, response_1, strlen(response_1), 0);
        
    } else if (response_num == 403) {
        response_2 = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        send(comm_fd, response_2, strlen(response_2), 0);
        warn("404 File %s Forbidden Access\n", resource_name);
        
    } else if (response_num == 404) {
        response_2 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        send(comm_fd, response_2, strlen(response_2), 0);
        warn("404 File %s Not Found\n", resource_name);
        
    } else if (response_num == 500) {
        response_2 = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        send(comm_fd, response_2, strlen(response_2), 0);
        warn("500 Internal Server Error While Opening %s For WRITE\n", resource_name);
        
    } else if (response_num == 200) {
        int k = snprintf(response_1, 200, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", content_len);
        response_1[k] = '\0';
        send(comm_fd, response_1, strlen(response_1), 0);
        
    } else if (response_num == 201) {
        response_2 = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
        send(comm_fd, response_2, strlen(response_2), 0);
    }
    
    return 0;
}

// source: section recording
unsigned long getaddr(char* name) {
    unsigned long res;
    struct addrinfo hints;
    struct addrinfo* info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &info) || info == NULL) {
        fprintf(stderr, "error finding %s\n", name);
        exit(1);
    }

    res = ((struct sockaddr_in*)info->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(info);
    return res;
}

/*
 * command line arguments: argv[1]: hostname/ipaddress, argv[2]: (optional) port (default=80)
 */
int main(int argc, char* argv[]) {
    char buf[SIZE];
    unsigned short port_number;
    char* address;
    if (argc == 2) {
        // only specified hostname/ipaddress
        address = argv[1];
        port_number = 80;
    }
    else if (argc == 3) {
        // both hostname/ipaddress and port specified
        address = argv[1];
        port_number = atoi(argv[2]);
    }
    else {
        fprintf(stderr, "Usage: %s <address> [port number]\n", argv[0]);
        exit(1);
    }

    // source: https://www.gta.ufrj.br/ensino/eel878/sockets/sockaddr_inman.html
    struct sockaddr_in myaddr; // sockaddr object
    memset(&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET; // set communication domain, AF_INET = IPv4 
    myaddr.sin_port = htons(port_number); // set port number
    myaddr.sin_addr.s_addr = getaddr(address); // assign address

    // creates socket, but has no address signed to it yet
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { // error handling
        warn("%s\n", argv[0]);
        exit(1);
    }
    // assigns myaddr address to socket specified by listen_fd
    int bind_success = bind(listen_fd, (struct sockaddr*) &myaddr, sizeof(myaddr));
    if (bind_success < 0) { // error handling
        warn("%s\n", argv[0]);
        exit(1);
    }

    // marks socket identified by listen_fd as a socket that can accept incoming connection requests
    int listen_success = listen(listen_fd, 500);
    if (listen_success < 0) { // error handling
        warn("%s\n", argv[0]);
        exit(1);
    }

    while (1) {
        // extracts the first connection request on the queue of pending connections for the listening socket
        int comm_fd = accept(listen_fd, NULL, NULL);
        if (comm_fd < 0) {
            warn("%s\n", argv[0]);
            exit(1);
        }

        // recieves requests/data from client using comm_fd connection
        while (1) {
            int n = recv(comm_fd, buf, SIZE, 0);
            if (n == 0) {
                break;
            }
            buf[n] = '\0';
            
            // parse the GET/PUT input
            struct header head = parseHeader(buf);
            
            // check if resource name is valid
            // length must = 11 (including the '/')
            if (strlen(head.resource_name) != 11) {
                // send 400 response
                send_response(comm_fd, 400, 0, head.resource_name);
                break;
            }
            
            // must contain alphanumeric characters
            bool contains_other = false;
            for (int i = 1; i < 11; i++) {
                if (isalnum(head.resource_name[i]) == 0) {
                    // send 400 response
                    send_response(comm_fd, 400, 0, head.resource_name);
                    contains_other = true;
                    break;
                }
            }
            
            if (contains_other) {
                break;
            }
                        
            // if put : save file and send status
            if (strcmp(head.command, "PUT") == 0) {
                // removes '/'
                if (head.resource_name[0] == '/') {
                    memmove(head.resource_name, head.resource_name+1, strlen(head.resource_name));
                }

                // opens file name for writing
                int open_fd = open(head.resource_name, O_RDWR | O_CREAT | O_TRUNC, 0667);
                if (open_fd < 0) {
                    if (errno == EACCES) {
                        send_response(comm_fd, 403, head.content_length, head.resource_name);
                        break;
                    } else {
                        // send 500 response
                        send_response(comm_fd, 500, head.content_length, head.resource_name);
                        break;
                    }
                }
                
                // if content length is specified
                if (head.content_length > -1) {
                    int content_size = head.content_length;
                    // while there is still content left to be read
                    while (content_size > 0) {
                        // read from client
                        n = recv(comm_fd, buf, SIZE, 0);
                        if (n <= 0) {
                            break;
                        }
                        // subtract number read of bytes from content_size
                        content_size -= n;

                        // write to file
                        write(open_fd, buf, n);
                    }

                    close(open_fd);
                    
                    // send 201 response
                    send_response(comm_fd, 201, head.content_length, head.resource_name);
                }
                else { // content length not specified, so read until EOF
                    // read from client
                    n = recv(comm_fd, buf, SIZE, 0);
                    // write to file until no bytes from input
                    while (n > 0) {
                        // write to file
                        write(open_fd, buf, n);
                        n = recv(comm_fd, buf, SIZE, 0);
                    }

                    close(open_fd);
                    
                    // send 201 response
                    send_response(comm_fd, 201, head.content_length, head.resource_name);
                }
            }
            // if get : retrieve file and send status and file
            else if (strcmp(head.command, "GET") == 0) {
                // removes '/'
                if (head.resource_name[0] == '/') {
                    memmove(head.resource_name, head.resource_name+1, strlen(head.resource_name));
                }

                int open_fd = open(head.resource_name, O_RDONLY);
                // send appropriate response
                if (open_fd < 0) {
                    // named file does not exist, send 404 response
                    if (errno == ENOENT) {
                        send_response(comm_fd, 404, head.content_length, head.resource_name);
                        break;
                    }
                    // named file cannot be opened due to permissions
                    else if (errno == EACCES) {
                        send_response(comm_fd, 403, head.content_length, head.resource_name);
                        break;
                    }
                    else {
                        // send 500 response
                        send_response(comm_fd, 500, head.content_length, head.resource_name);
                        break;
                    }
                }
                
                int content_len = 0;
                unsigned char* get_buffer = (unsigned char*) malloc(SIZE * sizeof(unsigned char));
                
                // Gets Content-Length before sending a response
                bool cannot_read = false;
                int size = read(open_fd, get_buffer, SIZE);
                while (size != 0) {
                    if (size < 0) {
                        send_response(comm_fd, 500, head.content_length, head.resource_name);
                        cannot_read = true;
                        break;
                    }
                                        
                    content_len += size;
                    
                    size = read(open_fd, get_buffer, SIZE);
                }
                
                close(open_fd);
                
                if (cannot_read) {
                    free(get_buffer);
                    break;
                }
                
                open_fd = open(head.resource_name, O_RDONLY);
                
                // Tells client how many bytes to expect
                send_response(comm_fd, 200, content_len, head.resource_name);

                // Sends the data of size Content-Length
                size = read(open_fd, get_buffer, SIZE);
                while (size != 0) {
                    if (size < 0) {
                        send_response(comm_fd, 500, head.content_length, head.resource_name);
                        cannot_read = true;
                        break;
                    }
                                        
                    n = send(comm_fd, get_buffer, size, 0);
                    if (n == -1) {
                        break;
                    }
                    
                    size = read(open_fd, get_buffer, SIZE);
                }

                close(open_fd);
                free(get_buffer);
            }

        }
        
        close(comm_fd);
    }

    return 0;
}
