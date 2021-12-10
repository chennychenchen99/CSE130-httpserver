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
#include <pthread.h>
#include <vector>
#include <queue>
#include <unordered_map> 
#include <string>
#include <dirent.h>
#include <unordered_set>

#define SMALL_BUF_SIZE 1
// each thread can allocate 16KiB of buffer space
#define BUFFER_SIZE 16384

using namespace std;

// http header
struct header {
    char* command;
    char* resource_name;
    int content_length;
};

// shared data amongst threads
struct shared_data {
    // conditional variable for worker threads
    pthread_cond_t worker_cond;
    // fileDescriptor for incoming connection requests
    int listen_fd;
    // queue to store communication requests
    queue<int> connections_queue;
    // mutex lock for connections_queue access
    pthread_mutex_t connections_queue_mutex;
    
    // maps file names to their respective mutex
    unordered_map<string, pthread_mutex_t> file_mutex_map;
    
    bool redundancy;
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
        warn("500 Internal Server Error While Opening %s\n", resource_name);
        
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

int compare_two_files(const char* file1, const char* file2) {
    int fd1 = open(file1, O_RDONLY);
    int fd2 = open(file2, O_RDONLY);
    
    if (fd1 < 0 || fd2 < 0) {
        return false;
    }
        
    char buf1[SMALL_BUF_SIZE];
    char buf2[SMALL_BUF_SIZE];
    
    long size1 = 1;
    long size2 = 1;
    
    // compares char by char
    while (size1 != 0 && size2 != 0) {
        size1 = read(fd1, buf1, SMALL_BUF_SIZE);
        size2 = read(fd2, buf2, SMALL_BUF_SIZE);
        
        if (size1 == 1 && size2 == 1) {
            if (buf1[0] != buf2[0]) {
                close(fd1);
                close(fd2);
                return false;
            }
        }
        
    }
    
    if (size1 != size2) {
        close(fd1);
        close(fd2);
        return false;
    }
    
    close(fd1);
    close(fd2);
    
    return true;
}

int get_which_file(const char* file1, const char* file2, const char* file3) {
    int status1 = compare_two_files(file1, file2);
    int status2 = compare_two_files(file1, file3);
    int status3 = compare_two_files(file2, file3);
    
    if (status1 == 1) {
        return 1; // file1 and file2 are equal, return 1 for file1
    } else if (status2 == 1) {
        return 1; // file1 and file3 are equal, return 1 for file1
    } else if (status3 == 1) {
        return 2; // file2 and file3 are equal, return 2 for file2
    }
    
    // None of the files are equal, so return -1 (error)
    return -1;
}

int handle_put(int comm_fd, char buf[], char* resource_name, int content_length, struct shared_data* shared) {
    // removes '/'
    if (resource_name[0] == '/') {
        memmove(resource_name, resource_name+1, strlen(resource_name));
    }

    // cast char* to std::string
    // file name 
    string file_name(resource_name);

    // gets mutex(es) and locks file(s)
    // if file hasnt been encountered before and isnt in the map, insert into map
    if (!shared->file_mutex_map.count(file_name)) {
        pthread_mutex_t file_mutex;
        pthread_mutex_init(&file_mutex, NULL);
        shared->file_mutex_map[file_name] = file_mutex;
    }
    // locks mutex for a file
    pthread_mutex_lock(&shared->file_mutex_map[file_name]);

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

    // unlocks mutex(es) for the file(s)
    pthread_mutex_unlock(&shared->file_mutex_map[file_name]);
    
    return 0;
}

int handle_put_redundancy(int comm_fd, char buf[], char* resource_name, int content_length, struct shared_data* shared) {
    // removes '/'
    if (resource_name[0] == '/') {
        memmove(resource_name, resource_name+1, strlen(resource_name));
    }

    // cast char* to std::string
    // file name
    string file_name(resource_name);
    // file names in case of redundancy
    string file1 = "copy1/" + file_name;
    string file2 = "copy2/" + file_name;
    string file3 = "copy3/" + file_name;
    
    // if copy[1-3]/file hasnt been seen yet, insert 3 new entries into map
    if (!shared->file_mutex_map.count(file1) || !shared->file_mutex_map.count(file2) || !shared->file_mutex_map.count(file3)) {
        pthread_mutex_t file_mutex1;
        pthread_mutex_init(&file_mutex1, NULL);
        shared->file_mutex_map[file1] = file_mutex1;
        pthread_mutex_t file_mutex2;
        pthread_mutex_init(&file_mutex2, NULL);
        shared->file_mutex_map[file2] = file_mutex2;
        pthread_mutex_t file_mutex3;
        pthread_mutex_init(&file_mutex3, NULL);
        shared->file_mutex_map[file3] = file_mutex3;
    }
    // locks mutex for 3 files
    pthread_mutex_lock(&shared->file_mutex_map[file1]);
    pthread_mutex_lock(&shared->file_mutex_map[file2]);
    pthread_mutex_lock(&shared->file_mutex_map[file3]);
    
    // opens file name for writing
    int open_fd;
    int open_fd_2;
    int open_fd_3;
    
    char file_name_1[50];
    char file_name_2[50];
    char file_name_3[50];
    
    sprintf(file_name_1, "copy1/%s", resource_name);
    sprintf(file_name_2, "copy2/%s", resource_name);
    sprintf(file_name_3, "copy3/%s", resource_name);
            
    open_fd = open(file_name_1, O_RDWR | O_CREAT | O_TRUNC, 0667);
    open_fd_2 = open(file_name_2, O_RDWR | O_CREAT | O_TRUNC, 0667);
    open_fd_3 = open(file_name_3, O_RDWR | O_CREAT | O_TRUNC, 0667);
    
    if ((open_fd < 0 && open_fd_2 < 0) || (open_fd < 0 && open_fd_3 < 0) ||
        (open_fd_2 < 0 && open_fd_3 < 0)) {
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

            // write to files
            write(open_fd, buf, n);
            write(open_fd_2, buf, n);
            write(open_fd_3, buf, n);
            
        }

        close(open_fd);
        close(open_fd_2);
        close(open_fd_3);
        
        // send 201 response
        send_response(comm_fd, 201, content_length, resource_name);
    } else { // content length not specified, so read until EOF
        // read from client
        int n = recv(comm_fd, buf, BUFFER_SIZE, 0);
        // write to file until no bytes from input
        while (n > 0) {
            // write to file
            write(open_fd, buf, n);
            write(open_fd_2, buf, n);
            write(open_fd_3, buf, n);
            
            n = recv(comm_fd, buf, BUFFER_SIZE, 0);
        }

        close(open_fd);
        close(open_fd_2);
        close(open_fd_3);
        
        // send 201 response
        send_response(comm_fd, 201, content_length, resource_name);
    }

    // unlocks mutex(es) for the file(s)
    pthread_mutex_unlock(&shared->file_mutex_map[file1]);
    pthread_mutex_unlock(&shared->file_mutex_map[file2]);
    pthread_mutex_unlock(&shared->file_mutex_map[file3]);
    
    return 0;
}

int handle_get(int comm_fd, char buf[], char* resource_name, int content_length, struct shared_data* shared) {
    // removes '/'
    if (resource_name[0] == '/') {
        memmove(resource_name, resource_name+1, strlen(resource_name));
    }
    
    // cast char* to std::string
    // file name 
    string file_name(resource_name);
    
    // if file hasnt been encountered before, then invalid GET request
    if (!shared->file_mutex_map.count(file_name)) {
        send_response(comm_fd, 404, content_length, resource_name);
        return -1;
    }
    // locks mutex for a file
    pthread_mutex_lock(&shared->file_mutex_map[file_name]);
    
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

    // unlocks mutex(es) for the file(s)
    pthread_mutex_unlock(&shared->file_mutex_map[file_name]);
    
    return 0;
}

int handle_get_redundancy(int comm_fd, char buf[], char* resource_name, int content_length, struct shared_data* shared) {
    // removes '/'
    if (resource_name[0] == '/') {
        memmove(resource_name, resource_name+1, strlen(resource_name));
    }
    
    // cast char* to std::string
    // file name
    string file_name(resource_name);
    // file names in case of redundancy
    string file1 = "copy1/" + file_name;
    string file2 = "copy2/" + file_name;
    string file3 = "copy3/" + file_name;
    
    if (!shared->file_mutex_map.count(file1) || !shared->file_mutex_map.count(file2) || !shared->file_mutex_map.count(file3)) {
        send_response(comm_fd, 404, content_length, resource_name);
        return -1;
    }
    // locks mutex for a file
    pthread_mutex_lock(&shared->file_mutex_map[file1]);
    pthread_mutex_lock(&shared->file_mutex_map[file2]);
    pthread_mutex_lock(&shared->file_mutex_map[file3]);
    
    int open_fd;
    int open_fd_2;
    int open_fd_3;
    char file_name_1[50];
    char file_name_2[50];
    char file_name_3[50];
    
    sprintf(file_name_1, "copy1/%s", resource_name);
    sprintf(file_name_2, "copy2/%s", resource_name);
    sprintf(file_name_3, "copy3/%s", resource_name);
    
    open_fd = open(file_name_1, O_RDONLY);
    open_fd_2 = open(file_name_2, O_RDONLY);
    open_fd_3 = open(file_name_3, O_RDONLY);
       
    // send appropriate response
    if ((open_fd < 0 && open_fd_2 < 0) || (open_fd < 0 && open_fd_3 < 0) ||
        (open_fd_2 < 0 && open_fd_3 < 0)) {
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
    
    int file_num = get_which_file(file_name_1, file_name_2, file_name_3);
    
    // by default open_fd is already file 1
    if (file_num == 2) {
        open_fd = open_fd_2;
    } else if (file_num < 0) {
        send_response(comm_fd, 500, content_length, resource_name);
        return -1;
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
    close(open_fd_2);
    close(open_fd_3);
    
    if (file_num == 1) {
        open_fd = open(file_name_1, O_RDONLY);
    } else { // file_num == 2
        open_fd = open(file_name_2, O_RDONLY);
    }
    
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
    
    pthread_mutex_unlock(&shared->file_mutex_map[file1]);
    pthread_mutex_unlock(&shared->file_mutex_map[file2]);
    pthread_mutex_unlock(&shared->file_mutex_map[file3]);
    
    return 0;
}

void* dispatcher(void* data) {
    struct shared_data* shared = (struct shared_data*) data;
    while (1) {
        // gets communication fd
        int comm_fd = accept(shared->listen_fd, NULL, NULL);

        // locks queue mutex for safe access
        pthread_mutex_lock(&shared->connections_queue_mutex);

        // pushes new communication to queue
        shared->connections_queue.push(comm_fd);
        
        // unlocks queue mutex so other threads can access
        pthread_mutex_unlock(&shared->connections_queue_mutex);
        
        // send a signal for a worker thread
        pthread_cond_signal(&shared->worker_cond);
    }
}

void* worker(void* data) {
    struct shared_data* shared = (struct shared_data*) data;
    // buffer for communication channel
    char comm_buffer[BUFFER_SIZE];
    while (1) {
        // locks queue mutex for safe access
        pthread_mutex_lock(&shared->connections_queue_mutex);

        // if the requests queue is empty, wait for signal from dispatch
        while (shared->connections_queue.empty()) {
            pthread_cond_wait(&shared->worker_cond, &shared->connections_queue_mutex);
        }

        // pop from queue the comm_fd that this thread will be using
        int comm_fd = shared->connections_queue.front();
        shared->connections_queue.pop();

        // printf("From thread with id %lu\n", pthread_self());

        // unlocks queue mutex
        pthread_mutex_unlock(&shared->connections_queue_mutex);
        
        // handles requests
        while (1) {
            int n = recv(comm_fd, comm_buffer, BUFFER_SIZE, 0);
            if (n == 0) {
                break;
            }
            comm_buffer[n] = '\0';
            
            // parse requests
            struct header head = parseHeader(comm_buffer);
            
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
                        
            // handle PUT/GET requests
            if (strcmp(head.command, "PUT") == 0) {
                int n;
                
                if (!shared->redundancy) {
                    n = handle_put(comm_fd, comm_buffer, head.resource_name, head.content_length, shared);
                } else {
                    n = handle_put_redundancy(comm_fd, comm_buffer, head.resource_name, head.content_length, shared);
                }
                
                if (n < 0) {
                    break;
                }
                
            } else if (strcmp(head.command, "GET") == 0) {
                int n;
                
                if (!shared->redundancy) {
                    n = handle_get(comm_fd, comm_buffer, head.resource_name, head.content_length, shared);
                } else {
                    n = handle_get_redundancy(comm_fd, comm_buffer, head.resource_name, head.content_length, shared);
                }
                
                if (n < 0) {
                    break;
                }
                
            } else {
                // if invalid request type
                send_response(comm_fd, 400, 0, head.resource_name);
                break;
            }
            
        }

    }
}

int main(int argc, char* argv[]) {
    // ======================================================================
    // process command line args
    // ======================================================================
    unsigned short port_number;
    unsigned short num_threads = 4;
    bool flag_redundancy = false;
    char* address;
    extern char *optarg;
    extern int optind, optopt;
    int c;
    if (argc >= 3) {
        // both hostname/ipaddress and port specified
        address = argv[1];
        port_number = atoi(argv[2]);
    }
    else if (argc >= 2) {
        // only specified hostname/ipaddress
        address = argv[1];
        port_number = 80;
    }
    else if (argc < 2){
        fprintf(stderr, "Usage: %s <address> [port number] [-r] [-N=<num_threads>]\n", argv[0]);
        exit(1);
    }
    
    // parses command line options -r and -N
    while ((c = getopt(argc, argv, "rN:")) != -1) {
        switch (c) {
            case 'r':
                flag_redundancy = true;
                break;
            case 'N':
                num_threads = atoi(optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s <address> [port number] [-r] [-N=<num_threads>]\n", argv[0]);
                exit(1);
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "Expected argument after options\n");
        exit(1);
    }

    // printf("address: %s, port: %d, -r: %d, -N: %d\n", address, port_number, flag_redundancy, num_threads);
    
    
    // ======================================================================
    //   Create socket
    // ======================================================================
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
    
    // initializes file_mutex_map by adding files that already exist in the server
    DIR *dir;
    struct dirent *ent;
    unordered_set<string> exclude_files;
    exclude_files.insert("httpserver");
    exclude_files.insert("httpserver.cpp");
    exclude_files.insert("httpserver.o");
    exclude_files.insert("Makefile");
    exclude_files.insert("DESIGN.pdf");
    exclude_files.insert("README.md");
    
    unordered_set<string> copy_dirs;
    copy_dirs.insert("copy1");
    copy_dirs.insert("copy2");
    copy_dirs.insert("copy3");
    
    unordered_map<string, pthread_mutex_t> file_mutex_map;

    bool copy_dirs_exist = false;
    dir = opendir("./");
    if (dir != NULL){
        while ((ent = readdir(dir))){
            // get file name
            string file_name = ent->d_name;

            // check if file_name isnt a assignemnt file
            if (!exclude_files.count(file_name) && file_name[0] != '.') {

                // if copy1, copy2, copy3 already exist, we need mutexes for files in them
                if (copy_dirs.count(file_name) && flag_redundancy) {
                    copy_dirs_exist = true;
                    string path = "./" + file_name + "/";
                    DIR * copy_dir = opendir(path.c_str());
                    struct dirent *copy_ent;
                    if (copy_dir != NULL) {
                        while ((copy_ent = readdir(copy_dir))) {
                            string copy_file_name = copy_ent->d_name;
                            if (copy_file_name[0] != '.') {
                                string temp_path = path + copy_file_name;
                                temp_path.erase(0, 2);
                                pthread_mutex_t file_mutex;
                                pthread_mutex_init(&file_mutex, NULL);
                                file_mutex_map[temp_path] = file_mutex;
                                // printf("In copy: %s\n", temp_path.c_str());
                            }
                        }
                    }
                    else {
                        fprintf(stderr, "Error opening copy directories: %d\n", errno);
                    }
                    closedir(copy_dir);
                } 
                else if (!copy_dirs.count(file_name) && !flag_redundancy) { // skip copy[1-3] when redundancy is not active
                    pthread_mutex_t file_mutex;
                    pthread_mutex_init(&file_mutex, NULL);
                    file_mutex_map[file_name] = file_mutex;
                    // printf("In cwd: %s\n", file_name.c_str());
                }
            }
        }
    }
    else {
        fprintf(stderr, "Error opening directory: %d\n", errno);
    }
    closedir(dir);

    // creates copy[1-3] dirs when there is a -r
    if (flag_redundancy and !copy_dirs_exist){
        mkdir("copy1", 0777);
        mkdir("copy2", 0777);
        mkdir("copy3", 0777);
    }

    // print file mutex map
    // for (auto it = file_mutex_map.cbegin(); it != file_mutex_map.cend(); ++it) {
    //     printf("In file mutex map: %s\n", (*it).first.c_str());
    // }

    // ======================================================================
    // Create threads
    // ======================================================================

    // dispatch thread
    pthread_t dispatch_thread;
    // vector for N worker threads
    vector<pthread_t> worker_threads(num_threads);

    // initialize shared data
    struct shared_data common_data;
    // initialize condition variables
    pthread_cond_init(&common_data.worker_cond, NULL);
    // queue for requests
    queue<int> connections_queue;
    common_data.connections_queue = connections_queue;
    // mutex for queue access
    pthread_mutex_init(&common_data.connections_queue_mutex, NULL);
    // socket fd
    common_data.listen_fd = listen_fd;

    // maps file names to their respective mutex
    common_data.file_mutex_map = file_mutex_map;

    // create main dispatcher thread
    pthread_create(&dispatch_thread, NULL, &dispatcher, &common_data);

    common_data.redundancy = flag_redundancy;
    
    // create N worker threads
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&worker_threads[i], NULL, &worker, &common_data) < 0) {
            fprintf(stderr, "Error creating thread\n");
            return 1;
        }
    }
    
    // ** Need to use join because a new thread was created for dispatcher **
    pthread_join(dispatch_thread, NULL);

    return 0;
}
