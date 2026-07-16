#include <sys/socket.h>   // socket(), bind(), listen(), setsockopt(), AF_INET, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR
#include <netinet/in.h>   // struct sockaddr_in, htons(), INADDR_ANY
#include <cstring>        // memset() — to zero out the sockaddr_in struct before filling it
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <iostream>

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
    fcntl(fd, F_SETFL, O_NONBLOCK);
    int epfd = epoll_create(1);
    
    struct epoll_event ev;

    ev.events =  EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    
    while(1) 
    {
        struct epoll_event events[16];
        int n = epoll_wait(epfd, events, 16, -1);
        std::cout << n << std::endl;
        for (int i = 0; i < n; i++) {
            if(events[i].data.fd == fd)
            {
                write(1, "listener ready\n", 15);
                struct epoll_event client_ev;
                int client_fd = accept(fd, NULL, NULL);
                fcntl(client_fd, F_SETFL, O_NONBLOCK);
                client_ev.events = EPOLLIN;
                client_ev.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);
            }
            
        }
    }
    return(0);
}