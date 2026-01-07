#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <regex.h>
#include <sys/time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <semaphore.h>
#include "queue.h"
#include "rwlock.h"
#include "asgn2_helper_funcs.h"
#define BUFSIZE 4096

int sock_fd = 0;
queue_t *queue;
rwlock_t *log_lock;
void *worker();

int send_response(
    int code, char *status_phrase, int connection_fd, int content_length, char *message_body) {
    // entire response
    char response[2049]; // = "HTTP/1.1 200 OK\r\nContent Length: ";
    memset(response, '\0', sizeof(response));
    strcat(response, "HTTP/1.1 ");

    // turn code number into string and cat
    char code_str[5];
    code_str[4] = '\0';
    sprintf(code_str, "%d ", code);
    strcat(response, code_str);

    // concat the status phrase given
    strcat(response, status_phrase);

    // the string for the length of the content (number only)
    char content_length_str[263];

    // concat the content length and the number
    memset(content_length_str, '\0', sizeof(content_length_str));
    sprintf(content_length_str, "\r\nContent-Length: %d\r\n\r\n", content_length);
    strcat(response, content_length_str);
    // strcat(response, "\r\n");
    strcat(response, message_body);

    // bro why does this fix it
    strcat(response, "\n");
    // write to the socket
    int res = 1;
    int totalBytesWritten = 0;
    int response_length = strlen(response) - 1;
    while (totalBytesWritten != response_length) {
        res = write(
            connection_fd, response + totalBytesWritten, response_length - totalBytesWritten);
        totalBytesWritten += res;
        if (res <= 0) {
            break;
        }
    }
    return res;
}

void handleCon(int connection_fd) {
    // blocks execution until connection is made

    // error out if bad connection
    if (connection_fd == -1) {
        return;
    }
    while (1) {
        // one extra for the null
        char buf[2049];
        memset(buf, '\0', sizeof(buf));

        ssize_t bytesRead = read_until(connection_fd, buf, 2048, "\r\n\r\n");

        if (!bytesRead) {
            char *message_body = "Bad Request\n";
            char *status_phrase = "Bad Request";
            send_response(400, status_phrase, connection_fd, strlen(message_body), message_body);
            fprintf(stderr, "NORESPONSE,/?,400,0\n");
            break;
        }

        // for debugging of course
        // parsing the string, it seems to work properly
        regex_t preg;
        regmatch_t pmatch[8];

        // can content have newline?? i think so, dont know how to fix that
        char *pattern = "^([a-zA-Z]{1,8}) (/[a-zA-Z0-9.-]{1,63}) "
                        "(HTTP/[0-9]\\.[0-9])\r\n([a-zA-Z0-9.-]{1,128}: [ "
                        "-~]{1,128}\r\n)*(Content-Length: [ "
                        "-~]{1,128}\r\n)?([a-zA-Z0-9.-]{1,128}: [ -~]{1,128}\r\n)*(\r\n)";

        if (regcomp(&preg, pattern, REG_EXTENDED | REG_NEWLINE) != 0) {
            fprintf(stderr, "Error compiling regex\n");
            regfree(&preg);
            exit(1);
        }

        if (regexec(&preg, buf, 8, pmatch, 0) != 0) {
            char *message_body = "Bad Request\n";
            char *status_phrase = "Bad Request";
            send_response(400, status_phrase, connection_fd, strlen(message_body), message_body);
            regfree(&preg);
            fprintf(stderr, "BADREQUEST,/?,400,0\n"); //idk about this
            break;
        }
        regfree(&preg);
        // get header request id
        char *header_location = strstr(buf, "\r\nRequest-Id: ");
        char header[129];
        memset(header, '\0', sizeof(header));
        header_location += strlen("\r\nRequest-Id: ");
        int req_dig = 0;
        int request_id = 0;
        for (int i = 0; i < 129 && *header_location != '\r'; i++) {
            if (!isdigit(*header_location)) {
                req_dig = 1;
                break;
            }
            header[i] = *header_location;
            header_location++;
        }
        if (!req_dig) {
            request_id = atoi(header);
        }

        // method GET or PUT
        char method[9]; // size is 8 + 1 (null) = 9
        memset(method, '\0', sizeof(method));
        strncpy(method, buf + pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so);

        // uri (file name)
        char uri[65]; // size is 64 + 1 (null) = 66
        memset(uri, '\0', sizeof(uri));
        // remove the slash at the front
        strncpy(uri, buf + pmatch[2].rm_so + 1, pmatch[2].rm_eo - pmatch[2].rm_so - 1);

        if (strcmp(method, "PUT") != 0 && strcmp(method, "GET") != 0) {
            char *message_body = "Not Implemented\n";
            char *status_phrase = "Not Implemented";
            send_response(501, status_phrase, connection_fd, strlen(message_body), message_body);
            fprintf(stderr, "UNSUPPORTED,/%s,501,%d\n", uri, request_id);
            break;
        }

        // get and check the HTTP version
        char version[9]; // size is 5 (HTTP/) + 1 (number) + 1(.) + 1 (number) + 1 (null) = 9
        memset(version, '\0', sizeof(version));
        strncpy(version, buf + pmatch[3].rm_so, pmatch[3].rm_eo - pmatch[3].rm_so);

        if (strcmp(version, "HTTP/1.1") != 0) {
            char *message_body = "Version Not Supported\n";
            char *status_phrase = "Version Not Supported";
            send_response(505, status_phrase, connection_fd, strlen(message_body), message_body);
            fprintf(stderr, "UNSUPPORTED,/%s,505,%d\n", uri, request_id);
            break;
        }

        int begin_length = pmatch[7].rm_eo;
        if (strcmp(method, "GET") == 0) {
            reader_lock(log_lock);
            int fd = open(uri, O_RDONLY);
            // check if its a directory
            struct stat s;
            if (stat(uri, &s) == 0 && S_ISDIR(s.st_mode)) {
                close(fd);
                char *message_body = "Forbidden\n";
                char *status_phrase = "Forbidden";
                send_response(
                    403, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "GET,/%s,403,%d\n", uri, request_id);
                reader_unlock(log_lock);
                break;
            }

            // check if you can access or if it exists
            if (fd == -1) {
                if (errno == EACCES) {
                    char *message_body = "Forbidden\n";
                    char *status_phrase = "Forbidden";
                    send_response(
                        403, status_phrase, connection_fd, strlen(message_body), message_body);
                    fprintf(stderr, "GET,/%s,403,%d\n", uri, request_id);
                    reader_unlock(log_lock);
                    break;
                }
                char *message_body = "Not Found\n";
                char *status_phrase = "Not Found";
                send_response(
                    404, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "GET,/%s,404,%d\n", uri, request_id);
                reader_unlock(log_lock);
                break;
            }

            long long file_size = s.st_size;

            // read the file from here and write to the request
            char *message_body = "";
            char *status_phrase = "OK";
            send_response(200, status_phrase, connection_fd, file_size, message_body);
            struct timeval timeout;

            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            setsockopt(
                connection_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout, sizeof(timeout));
            pass_n_bytes(fd, connection_fd, 2147483647);

            fprintf(stderr, "GET,/%s,200,%d\n", uri, request_id);
            reader_unlock(log_lock);
        }

        else if (strcmp(method, "PUT") == 0) {
            regex_t preg;
            regmatch_t pmatch[7];

            char *pattern = "^([a-zA-Z]{1,8}) (/[a-zA-Z0-9.-]{1,63}) "
                            "(HTTP/[0-9]\\.[0-9])\r\n([a-zA-Z0-9.-]{1,128}: [ "
                            "-~]{1,128}\r\n)*(Content-Length: [ "
                            "-~]{1,128}\r\n){1}([a-zA-Z0-9.-]{1,128}: [ -~]{1,128}\r\n)*\r\n";

            if (regcomp(&preg, pattern, REG_EXTENDED | REG_NEWLINE) != 0) {
                fprintf(stderr, "Error compiling regex\n");
                regfree(&preg);
                exit(1);
            }

            if (regexec(&preg, buf, 7, pmatch, 0) != 0) {
                char *message_body = "Bad Request\n";
                char *status_phrase = "Bad Request";
                send_response(
                    400, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "PUT,/%s,400,%d\n", uri, request_id);
                regfree(&preg);
                break;
            }
            regfree(&preg);

            // check for content-length
            char *content_length = buf + pmatch[5].rm_so;

            // get towards the number
            content_length += 16;

            //string to turn into int
            char content_length_str[129];
            memset(content_length_str, '\0', sizeof(content_length_str));

            // loop until end
            int notdigit = 0;
            for (int i = 0; *content_length != '\r'; content_length++) {
                // bad request if content length is not a digit
                if (!isdigit(*content_length)) {
                    char *message_body = "Bad Request\n";
                    char *status_phrase = "Bad Request";
                    send_response(
                        400, status_phrase, connection_fd, strlen(message_body), message_body);
                    notdigit = 1;
                    fprintf(stderr, "PUT,/%s,400,%d\n", uri, request_id);
                    break;
                }
                content_length_str[i] = *content_length;
                i++;
            }
            if (notdigit) {
                char *message_body = "Bad Request\n";
                char *status_phrase = "Bad Request";
                send_response(
                    400, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "PUT,/%s,400,%d\n", uri, request_id);
                break;
            }
            // turn into an int
            int content_length_int = atoi(content_length_str);

            int created = 0;
            writer_lock(log_lock);
            int fd = open(uri, O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (fd == -1) {
                created = 1;
                fd = open(uri, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            }

            struct stat s;
            if (stat(uri, &s) == 0 && S_ISDIR(s.st_mode)) {
                close(fd);
                char *message_body = "Forbidden\n";
                char *status_phrase = "Forbidden";
                send_response(
                    403, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "PUT,/%s,403,%d\n", uri, request_id);
                writer_unlock(log_lock);
                break;
            }

            int bytesWritten = write_n_bytes(fd, buf + begin_length, bytesRead - begin_length);

            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            setsockopt(
                connection_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &timeout, sizeof(timeout));

            pass_n_bytes(connection_fd, fd, content_length_int - bytesWritten);

            if (created) {
                char *message_body = "Created\n";
                char *status_phrase = "Created";
                send_response(
                    201, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "PUT,/%s,201,%d\n", uri, request_id);
            } else {
                char *message_body = "OK\n";
                char *status_phrase = "OK";
                send_response(
                    200, status_phrase, connection_fd, strlen(message_body), message_body);
                fprintf(stderr, "PUT,/%s,200,%d\n", uri, request_id);
            }
            writer_unlock(log_lock);
            close(fd);
        }
        break;
    }
    char buf[BUFSIZE];
    while (read_n_bytes(connection_fd, buf, BUFSIZE)) {
        ;
    }
}

void *worker() {
    while (1) {
        int conn_fd = -1;
        queue_pop(queue, (void *) (int *) &conn_fd);
        handleCon(conn_fd);
        close(conn_fd);
    }
}

int main(int argc, char *argv[]) {
    // check num of args
    if (argc < 2) {
        fprintf(stderr, "Bad number of arguments\n");
        exit(1);
    }
    // use getopt to check if number of threads is valid
    int opt = 0;
    int threads = 4; // number of threads
    while ((opt = getopt(argc, argv, "t:"))) {
        if (opt == 't') {
            threads = atoi(optarg);
            if (threads < 0) {
                fprintf(stderr, "Bad num of threads\n");
                exit(1);
            }
            break;
        } else {
            break;
        }
    }
    char *temp = NULL;
    int port = strtoull(argv[optind], &temp, 10);
    if (temp && *temp != '\0') {
        fprintf(stderr, "Invalid Port\n");
        exit(1);
    }
    if (optind + 1 < argc) {
        fprintf(stderr, "Bad number of arguments\n");
        exit(1);
    }

    signal(SIGPIPE, SIG_IGN);

    Listener_Socket sock;
    // create listener
    int init = listener_init(&sock, port);

    // error if it cannot listen to the port for some reason
    if (init == -1) {
        fprintf(stderr, "Invalid Port\n");
        exit(1);
    }
    sock_fd = sock.fd;

    // init new thread
    queue = queue_new(threads);

    // init new rwlock
    log_lock = rwlock_new(N_WAY, 10);

    pthread_t thread_arr[threads];
    for (int i = 0; i < threads; i++) {
        pthread_create(&(thread_arr[i]), NULL, worker, NULL);
    }

    // dispatcher
    while (1) {
        // fprintf(stderr, "Listening?\n");
        int conn_fd = listener_accept(&sock);
        // fprintf(stderr, "Listened\n");
        queue_push(queue, (void *) (uintptr_t) conn_fd);
    }
    return 0;
}
