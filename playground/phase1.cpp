#include <sys/socket.h>   // socket(), bind(), listen(), setsockopt(), AF_INET, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR
#include <netinet/in.h>   // struct sockaddr_in, htons(), INADDR_ANY
#include <cstring>        // memset() — to zero out the sockaddr_in struct before filling it
#include <unistd.h>       // close() — you'll need it soon, for when a call fails or at shutdown

int main(int argc, char **argv) {

    (void)argc;
    (void)argv;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == -1)
    {
        return (1);
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)); // check retun value
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        return(1);
    }
    listen(fd, 16); // check return value
    while (1)
    {
        char buf[1024];
        int client_fd = accept(fd, NULL, NULL);
        if( client_fd == -1)
            continue;
        int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            write(1, buf, n);
        }
        char response[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, world!";
        send(client_fd, response, sizeof(response) -1, 0);
        close(client_fd);
    }
    return (0);
}