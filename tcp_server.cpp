#include "common.h"
#include <string>
#include <sys/epoll.h>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#define SERV_PORT   19999
#define MAXLINE     1024
#define MAXSIZE     1024
#define EPOLLEVENTS 100
#define FDSIZE      20
using namespace std;

#define ETH_NAME     "enp3s0"
#define REQUEST_INFO "new_client_ip"  //客户端发送的广播信息头
#define REPLAY_INFO  "server_ip"      //服务端回复的信息头
#define INFO_SPLIT   std::string(":") //信息分割符

//对c字符串按照指定分割符拆分为多个string字符串
void cstr_split(char *cstr, vector<std::string> &res, std::string split = INFO_SPLIT)
{
    res.clear();
    char *token = strtok(cstr, split.c_str());
    while (token) {
        res.push_back(std::string(token));
        printf("[%s] token:%s\n", __func__, token);
        token = strtok(NULL, split.c_str());
    }
}

//获取本机ip(根据实际情况修改ETH_NAME)
bool get_local_ip(std::string &ip)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        printf("[%s] socket err!\n", __func__);
        return false;
    }

    struct ifreq ifr;
    memcpy(&ifr.ifr_name, ETH_NAME, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = 0;
    if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
        printf("[%s] ioctl err!\n", __func__);
        return false;
    }

    struct sockaddr_in sin;
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    ip = std::string(inet_ntoa(sin.sin_addr));
    return true;
}

//设置该套接字为广播类型
void set_sockopt_broadcast(int socket, bool bEnable = true)
{
    const int opt = (int)bEnable;
    int       nb  = setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
    if (nb == -1) {
        printf("[%s] set socket error\n", __func__);
        return;
    }
}

//接收客户端广播信息的处理线程, 收到客户端的UDP广播后, 将自己(服务端)的IP发送回去
void recv_broadcast_thread(void)
{
    std::string localIP = "";
    if (true == get_local_ip(localIP)) {
        printf("[%s] localIP: [%s] %s\n", __func__, ETH_NAME, localIP.c_str());
    } else {
        printf("[%s] get local ip err!\n", __func__);
        return;
    }

    int sock = -1;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        printf("[%s] socket error\n", __func__);
        return;
    }

    struct sockaddr_in udpServerAddr;
    bzero(&udpServerAddr, sizeof(struct sockaddr_in));
    udpServerAddr.sin_family      = AF_INET;
    udpServerAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    udpServerAddr.sin_port        = htons(6000);
    int len                       = sizeof(sockaddr_in);

    if (bind(sock, (struct sockaddr *)&(udpServerAddr), sizeof(struct sockaddr_in)) == -1) {
        printf("[%s] bind error\n", __func__);
        return;
    }

    set_sockopt_broadcast(sock);

    char smsg[100] = { 0 };

    while (1) {
        //从广播地址接收消息
        int ret =
            recvfrom(sock, smsg, 100, 0, (struct sockaddr *)&udpServerAddr, (socklen_t *)&len);
        if (ret <= 0) {
            printf("[%s] read error, ret:%d\n", __func__, ret);
        } else {
            printf("[%s]receive: %s\n", __func__, smsg);

            vector<std::string> recvInfo;
            cstr_split(smsg, recvInfo);

            //将自己的IP回应给请求的客户端
            if (recvInfo.size() == 2 && recvInfo[0] == REQUEST_INFO) {
                std::string clientIP  = recvInfo[1];
                std::string replyInfo = REPLAY_INFO + INFO_SPLIT + localIP;

                ret = sendto(sock,
                             replyInfo.c_str(),
                             replyInfo.length(),
                             0,
                             (struct sockaddr *)&udpServerAddr,
                             len);
                if (ret < 0) {
                    printf("[%s] sendto error, ret: %d\n", __func__, ret);
                } else {
                    printf("[%s] reply ok, msg: %s\n", __func__, replyInfo.c_str());
                }
            }
        }

        sleep(1);
    }
}

//为epoll中的某个fd添加/修改/删除某个事件
bool epoll_set_fd_a_event(int epollfd, int op, int fd, int event)
{
    if (EPOLL_CTL_ADD == op || EPOLL_CTL_MOD == op || EPOLL_CTL_DEL == op) {
        struct epoll_event ev;
        ev.events  = event;
        ev.data.fd = fd;
        epoll_ctl(epollfd, op, fd, &ev);
        return true;
    } else {
        printf("[%s] err op:%d\n", __func__, op);
        return false;
    }
}

//TCP服务器线程, 用于接受客户端的连接, 并接收客户端的信息
void tcp_loop_thread(void)
{
    //创建服务器端套接字文件
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    //初始化服务器端口地址
    struct sockaddr_in tcp_server_addr;
    bzero(&tcp_server_addr, sizeof(tcp_server_addr));
    tcp_server_addr.sin_family      = AF_INET;
    tcp_server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    tcp_server_addr.sin_port        = htons(SERV_PORT);

    //将套接字文件与服务器端口地址绑定
    bind(listenfd, (struct sockaddr *)&tcp_server_addr, sizeof(tcp_server_addr));

    //监听，并设置最大连接数为20
    listen(listenfd, 20);
    printf("[%s] Accepting connections... \n", __func__);

    //通过epoll来监控多个客户端的请求
    int                epollfd;
    struct epoll_event events[EPOLLEVENTS];
    int                num;
    char               buf[MAXSIZE];
    memset(buf, 0, MAXSIZE);
    epollfd = epoll_create(FDSIZE);
    printf("[%s] create epollfd:%d\n", __func__, epollfd);

    //添加监听描述符事件
    epoll_set_fd_a_event(epollfd, EPOLL_CTL_ADD, listenfd, EPOLLIN);
    while (1) {
        //获取已经准备好的描述符事件
        printf("[%s] epollfd:%d epoll_wait...\n", __func__, epollfd);
        num = epoll_wait(epollfd, events, EPOLLEVENTS, -1);
        for (int i = 0; i < num; i++) {
            int fd = events[i].data.fd;
            //listenfd说明有新的客户端请求连接
            if ((fd == listenfd) && (events[i].events & EPOLLIN)) {
                //accept客户端的请求
                struct sockaddr_in cliaddr;
                socklen_t          cliaddrlen = sizeof(cliaddr);
                int clifd = accept(listenfd, (struct sockaddr *)&cliaddr, &cliaddrlen);
                if (clifd == -1) {
                    perror("accpet error:");
                } else {
                    printf("[%s] accept a new client(fd:%d): %s:%d\n",
                           __func__,
                           clifd,
                           inet_ntoa(cliaddr.sin_addr),
                           cliaddr.sin_port);
                    //将客户端fd添加到epoll进行监听
                    epoll_set_fd_a_event(epollfd, EPOLL_CTL_ADD, clifd, EPOLLIN);
                }
            }
            //收到已连接的客户端fd的消息
            else if (events[i].events & EPOLLIN) {
                memset(buf, 0, MAXSIZE);
                //读取客户端的消息
                int nread = read(fd, buf, MAXSIZE);
                if (nread == -1) {
                    perror("read error:");
                    close(fd);
                    epoll_set_fd_a_event(epollfd, EPOLL_CTL_DEL, fd, EPOLLIN);
                } else if (nread == 0) {
                    printf("[%s] client(fd:%d) close.\n", __func__, fd);
                    close(fd);
                    epoll_set_fd_a_event(epollfd, EPOLL_CTL_DEL, fd, EPOLLIN);
                } else {
                    //将客户端的消息打印处理, 并表明是哪里客户端fd发来的消息
                    printf("[%s] read message from fd:%d ---> %s\n", __func__, fd, buf);
                }
            }
        }
    }

    close(epollfd);
}

int main(void)
{
    thread th1(recv_broadcast_thread);
    thread th2(tcp_loop_thread);
    th1.join();
    th2.join();

    return 0;
}