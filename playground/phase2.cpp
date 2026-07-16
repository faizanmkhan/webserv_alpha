#include <sys/socket.h>   // socket(), bind(), listen(), setsockopt(), AF_INET, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR
#include <netinet/in.h>   // struct sockaddr_in, htons(), INADDR_ANY
#include <cstring>        // memset() — to zero out the sockaddr_in struct before filling it
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <iostream>
#include <map>
#include <string>

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
    std::map<int, std::string> pending_writes;
    while(1) 
    {
        struct epoll_event events[16];
        int n = epoll_wait(epfd, events, 16, -1);
        std::cout << n << std::endl;
        for (int i = 0; i < n; i++) {
            if(events[i].data.fd == fd)
            {
                int client_fd = accept(fd, NULL, NULL);
                //write(1, "listener ready\n", 15);
                struct epoll_event client_ev;
                fcntl(client_fd, F_SETFL, O_NONBLOCK);
                client_ev.events = EPOLLIN;
                client_ev.data.fd = client_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);
            }
            else if (events[i].events & EPOLLIN)
            {
                char buf[1024];
                int read_byte = recv(events[i].data.fd, buf, sizeof(buf), 0);
                if (read_byte > 0) {
                    pending_writes[events[i].data.fd] = std::string(buf, n);
                    struct epoll_event client_wr_ev;
                    client_wr_ev.events = EPOLLOUT;
                    client_wr_ev.data.fd = events[i].data.fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, events[i].data.fd, &client_wr_ev);
                    
                }
                else {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                    close(events[i].data.fd);
                }
                //char response[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, world!";
            }
            else if(events[i].events & EPOLLOUT)
            {
                std::string data = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, world!";
                send(events[i].data.fd, data.c_str(), data.size(), 0);
                pending_writes.erase(events[i].data.fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, NULL);
                close(events[i].data.fd);
            }
            
        }
       
    }
    return(0);
}