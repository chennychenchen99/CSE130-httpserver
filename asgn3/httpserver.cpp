#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#include <time.h>
#include <dirent.h>

#define BUFFER_SIZE 16384

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

int handle_put(int comm_fd, char buf[], char* resource_name, int content_length) {
    // removes '/'
    if (resource_name[0] == '/') {
        memmove(resource_name, resource_name+1, strlen(resource_name));
    }

    // opens file name for writing
    int open_fd;
    
    open_fd = open(resource_name, O_RDWR | O_CREAT | O_TRUNC, 0667);
    
    if (open_fd < 0) {
        if (errno == EACCES) {
            send_response(comm_fd, 403, content_length, resource_name);
            return -1;
        } else {
            // send 500 response
            send_response(comm_fd, 500, content_length, resource_name);
            return -1;
        }
    }
    
    // if content length is specified
    if (content_length > -1) {
        int content_size = content_length;
        // while there is still content left to be read
        while (content_size > 0) {
            // read from client
            int n = recv(comm_fd, buf, BUFFER_SIZE, 0);
            if (n <= 0) {
                return -1;
            }
            // subtract number read of bytes from content_size
            content_size -= n;

            // write to file
            write(open_fd, buf, n);
        }

        close(open_fd);
        
        // send 201 response
        send_response(comm_fd, 201, content_length, resource_name);
    } else { // content length not specified, so read until EOF
        // read from client
        int n = recv(comm_fd, buf, BUFFER_SIZE, 0);
        // write to file until no bytes from input
        while (n > 0) {
            // write to file
            write(open_fd, buf, n);
            
            n = recv(comm_fd, buf, BUFFER_SIZE, 0);
        }

        close(open_fd);
        
        // send 201 response
        send_response(comm_fd, 201, content_length, resource_name);
    }
    return 0;
}

int handle_get(int comm_fd, char buf[], char* resource_name, int content_length) {
    // removes '/'
    if (resource_name[0] == '/') {
        memmove(resource_name, resource_name+1, strlen(resource_name));
    }
    
    int open_fd;
    
    open_fd = open(resource_name, O_RDONLY);
        
    // send appropriate response
    if (open_fd < 0) {
        // named file does not exist, send 404 response
        if (errno == ENOENT) {
            send_response(comm_fd, 404, content_length, resource_name);
            return -1;
        }
        // named file cannot be opened due to permissions
        else if (errno == EACCES) {
            send_response(comm_fd, 403, content_length, resource_name);
            return -1;
        }
        else {
            // send 500 response
            send_response(comm_fd, 500, content_length, resource_name);
            return -1;
        }
    }
    
    int content_len = 0;
    unsigned char* get_buffer = (unsigned char*) malloc(BUFFER_SIZE * sizeof(unsigned char));
        
    // Gets Content-Length before sending a response
    int size = read(open_fd, get_buffer, BUFFER_SIZE);
    
    while (size != 0) {
        if (size < 0) {
            send_response(comm_fd, 500, content_length, resource_name);
            free(get_buffer);
            return -1;
        }
                            
        content_len += size;
        
        size = read(open_fd, get_buffer, BUFFER_SIZE);
    }
    
    close(open_fd);
        
    open_fd = open(resource_name, O_RDONLY);
        
    // Tells client how many bytes to expect
    send_response(comm_fd, 200, content_len, resource_name);

    // Sends the data of size Content-Length
    size = read(open_fd, get_buffer, BUFFER_SIZE);
    while (size != 0) {
        if (size < 0) {
            send_response(comm_fd, 500, content_length, resource_name);
            return -1;
        }
                                    
        int n = send(comm_fd, get_buffer, size, 0);
        if (n == -1) {
            return -1;
        }
        
        size = read(open_fd, get_buffer, BUFFER_SIZE);
    }

    close(open_fd);
    free(get_buffer);
    
    return 0;
}

int handle_backup(int comm_fd, char buf[], char* resource_name, int content_length) {
    time_t seconds = time(NULL);
    
    // Create backup folder name
    char backup_dir[500];
    int n = snprintf(backup_dir, 500, "backup-%ld", seconds);
    backup_dir[n] = '\0';
    
    mkdir(backup_dir, 0777);
    
    DIR *directory;
    struct dirent *file;
    
    directory = opendir(".");
    
    // Goes through each file in the directory
    while ((file = readdir(directory))) {
        char* filename = file->d_name;
        
        // If filename is not a folder and length of filename is 10, add a copy to the backup folder
        if (strlen(filename) == 10 && file->d_type != DT_DIR && (filename[8] != '.' && (filename[9] != 'o' || filename[9] != 'c'))) {
            char new_filename[500];
            int n = snprintf(new_filename, 500, "%s/%s", backup_dir, filename);
            new_filename[n] = '\0';
            
            // open current file
            int open_fd = open(filename, O_RDONLY);
            
            if (open_fd < 0 && errno == EACCES) {
                continue;
            }
            
            // create the new file in the backup directory
            int new_open_fd = open(new_filename, O_RDWR | O_CREAT | O_TRUNC, 0667);
            
            unsigned char* get_buffer = (unsigned char*) malloc(BUFFER_SIZE * sizeof(unsigned char));
            
            int size = read(open_fd, get_buffer, BUFFER_SIZE);
            while (size != 0) {
                if (size < 0) {
                    send_response(comm_fd, 500, content_length, resource_name);
                    free(get_buffer);
                    return -1;
                }
                
                write(new_open_fd, get_buffer, size);
                
                size = read(open_fd, get_buffer, BUFFER_SIZE);
            }
            
            free(get_buffer);
            close(open_fd);
            close(new_open_fd);
        }
    }
    
    // send 201 response
    send_response(comm_fd, 201, content_length, resource_name);
    
    closedir(directory);
    return 0;
}

int handle_recovery(int comm_fd, char buf[], char* resource_name, int content_length) {
    DIR *directory;
    struct dirent *file;
    char backup_dir[500];

    // get the most recent backup directory
    if (strlen(resource_name) == 2) {
        DIR *curdir;
        struct dirent *curdirfile;

        curdir = opendir(".");
        long int timestamp = 0;

        // Goes through each file in the directory
        while ((curdirfile = readdir(curdir))) {
            char* filename = curdirfile->d_name;
            // skip . and ..
            if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 || curdirfile->d_type != DT_DIR || strlen(filename) < 8) {
                continue;
            }
            
            char substr[10];
            strncpy(substr, filename, 7);
            substr[7] = '\0';
            
            if (strcmp(substr, "backup-") == 0) {
                int k = strlen(filename);
                k = k-7;
                char time_str[20];
                strncpy(time_str, filename+7, k);
                char* eptr;
                long int cur_timestamp = strtol(time_str, &eptr, 10);
                if (cur_timestamp > timestamp) {
                    timestamp = cur_timestamp;
                }
            }
        }
        
        closedir(curdir);

        int n = snprintf(backup_dir, 500, "./backup-%ld", timestamp);
        backup_dir[n] = '\0';

        directory = opendir(backup_dir);
        
        if (directory == NULL) {
            if (errno == ENOENT) {
                // directory doesn't exist
                send_response(comm_fd, 404, content_length, resource_name);
                return -1;
            } else if (errno == EACCES) {
                // directory doesn't have permission to open
                send_response(comm_fd, 403, content_length, resource_name);
                return -1;
            } else {
                send_response(comm_fd, 500, content_length, resource_name);
                return -1;
            }
        }
        
    } else { // get backup directory specified by resource name (timestamp)
        // removes '/r/'
        if (resource_name[0] == '/') {
            memmove(resource_name, resource_name+3, strlen(resource_name));
        }

        for (int i = 0; i < strlen(resource_name); i++) {
            if (!isdigit(resource_name[i])) {
                send_response(comm_fd, 400, 0, resource_name);
                return -1;
            }
        }

        int n = snprintf(backup_dir, 500, "./backup-%s", resource_name);
        backup_dir[n] = '\0';
        
        directory = opendir(backup_dir);

        if (directory == NULL) {
            if (errno == ENOENT) {
                // directory doesn't exist
                send_response(comm_fd, 404, content_length, resource_name);
                return -1;
            } else if (errno == EACCES) {
                // directory doesn't have permission to open
                send_response(comm_fd, 403, content_length, resource_name);
                return -1;
            } else {
                send_response(comm_fd, 500, content_length, resource_name);
                return -1;
            }
        }
    }

    // Goes through each file in the backup directory
    while ((file = readdir(directory))) {
        char* filename = file->d_name;
        // skip . and ..
        if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
            continue;
        }
        
        // copy file in backup dir into server dir
        char backup_filename[500];
        int n = snprintf(backup_filename, 500, "%s/%s", backup_dir, filename);
        backup_filename[n] = '\0';

        int backup_fd = open(backup_filename, O_RDONLY);
        int copy_fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0667);

        unsigned char* get_buffer = (unsigned char*) malloc(BUFFER_SIZE * sizeof(unsigned char));
        
        int size = read(backup_fd, get_buffer, BUFFER_SIZE);
        while (size != 0) {
            if (size < 0) {
                break;
            }
            
            write(copy_fd, get_buffer, size);
            
            size = read(backup_fd, get_buffer, BUFFER_SIZE);
        }
        
        free(get_buffer);
        close(backup_fd);
        close(copy_fd);
    }
    
    // send 201 response
    send_response(comm_fd, 201, content_length, resource_name);
    
    closedir(directory);
    return 0;
}

int handle_list(int comm_fd, char buf[], char* resource_name, int content_length) {
    DIR *dir;
    struct dirent *file;
    bool contains_no_backups = true;
    int content_len = 0;

    dir = opendir(".");

    // Goes through each file in the directory and count the content length
    while ((file = readdir(dir))) {
        char* filename = file->d_name;

        // skip . and .., and skip non-dirs, as well as dirs with name < length 8
        if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 || file->d_type != DT_DIR || strlen(filename) < 8) {
            continue;
        }
        
        char substr[10];
        strncpy(substr, filename, 7);
        substr[7] = '\0';
        
        // finds dirs starting with 'backup-'
        if (strcmp(substr, "backup-") == 0) {
            int k = strlen(filename);
            k = k-7;
            char time_str[20];
            strncpy(time_str, filename+7, k);
            time_str[k] = '\0';
            bool is_valid_timestamp = true;
            // gets rid of invalid backup timestamp strings
            for (int i = 0; i < strlen(time_str); i++) {
                if (!isdigit(time_str[i])) {
                    is_valid_timestamp = false;
                    break;
                }
            }
            
            if (is_valid_timestamp) {
                char timestamp[20];
                int n = snprintf(timestamp, 20, "%s\n", filename+7);
                content_len += n;
                contains_no_backups = false;
            }
        }
    }
    closedir(dir);

    if (contains_no_backups) {
        // send 200 response with content length: 0 if no backups are available
        send_response(comm_fd, 200, 0, resource_name);
    } else {
        send_response(comm_fd, 200, content_len, resource_name);
        
        dir = opendir(".");

        // Goes through each file in the directory and sends the timestamps
        while ((file = readdir(dir))) {
            char* filename = file->d_name;

            // skip . and .., and skip non-dirs, as well as dirs with name < length 8
            if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 || file->d_type != DT_DIR || strlen(filename) < 8) {
                continue;
            }
            
            char substr[10];
            strncpy(substr, filename, 7);
            substr[7] = '\0';
            
            // finds dirs starting with 'backup-'
            if (strcmp(substr, "backup-") == 0) {
                int k = strlen(filename);
                k = k-7;
                
                char time_str[20];
                strncpy(time_str, filename+7, k);
                time_str[k] = '\0';
                bool is_valid_timestamp = true;
                // gets rid of invalid backup timestamp strings
                for (int i = 0; i < strlen(time_str); i++) {
                    if (!isdigit(time_str[i])) {
                        is_valid_timestamp = false;
                        break;
                    }
                }

                if (is_valid_timestamp) {
                    char timestamp[20];
                    int n = snprintf(timestamp, 20, "%s\n", filename+7);
                    int j = send(comm_fd, timestamp, strlen(timestamp), 0);
                }
            }
        }
        closedir(dir);
        
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
    char buf[BUFFER_SIZE];
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
            /*
             * cmd_type
             * 0: regular GET
             * 1: backup
             * 2: recovery
             * 3: list
             */
            int cmd_type = 0;
            
            int n = recv(comm_fd, buf, BUFFER_SIZE, 0);
            if (n == 0) {
                break;
            }
            buf[n] = '\0';
            
            // parse the GET/PUT input
            struct header head = parseHeader(buf);
            
            // first 3 if statements checks for /b, /r, /l
            if (strcmp(head.command, "GET") == 0 && strlen(head.resource_name) == 2 && head.resource_name[1] == 'b') {
                cmd_type = 1;
            } else if (strcmp(head.command, "GET") == 0 &&
                ((strlen(head.resource_name) == 2 && head.resource_name[1] == 'r') ||
                (head.resource_name[1] == 'r' && head.resource_name[2] == '/' && strlen(head.resource_name) > 3))) { // in the case of /r/[timestamp]
                cmd_type = 2;
            } else if (strcmp(head.command, "GET") == 0 && strlen(head.resource_name) == 2 && head.resource_name[1] == 'l') {
                cmd_type = 3;
            } else {
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
            }
            
            // if put : save file and send status
            if (strcmp(head.command, "PUT") == 0) {
                int n = handle_put(comm_fd, buf, head.resource_name, head.content_length);
                if (n != 0) {
                    break;
                }
            }
            // if get : retrieve file and send status and file
            else if (strcmp(head.command, "GET") == 0) {
                int n;
                
                if (cmd_type == 1) {
                    n = handle_backup(comm_fd, buf, head.resource_name, head.content_length);
                } else if (cmd_type == 2) {
                    n = handle_recovery(comm_fd, buf, head.resource_name, head.content_length);
                } else if (cmd_type == 3) {
                    n = handle_list(comm_fd, buf, head.resource_name, head.content_length);
                } else {
                    n = handle_get(comm_fd, buf, head.resource_name, head.content_length);
                }
                
                if (n != 0) {
                    break;
                }
            } else {
                // if invalid request type
                send_response(comm_fd, 400, 0, head.resource_name);
                break;
            }

        }
        
        close(comm_fd);
    }

    return 0;
}
