#include "common.h"
#include <string>
#include <sys/epoll.h>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#define SERV_PORT   19999
#define MAXLINE     1024
#define MAXSIZE     1024
#define EPOLLEVENTS 100
#define FDSIZE      20
using namespace std;
using json = nlohmann::json;
#define ETH_NAME     "ens33"
#define REQUEST_INFO "new_client_ip"  //客户端发送的广播信息头
#define REPLAY_INFO  "server_ip"      //服务端回复的信息头
#define INFO_SPLIT   std::string(":") //信息分割符

enum msgType {
    CHECK_ONLINE = 0x00,
    CLIENT_REQ_IP,
};


void tcp_client_thread(std::string serverIP, std::string localIP)
{
    printf("[%s] in, prepare connect serverIP:%s\n", __func__, serverIP.c_str());

    //创建客户端套接字文件
    int tcpClientSocket = socket(AF_INET, SOCK_STREAM, 0);

    //初始化服务器端口地址
    struct sockaddr_in servaddr;
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, serverIP.c_str(), &servaddr.sin_addr);
    servaddr.sin_port = htons(SERV_PORT);

    //请求连接
    connect(tcpClientSocket, (struct sockaddr *)&servaddr, sizeof(servaddr));

    //要向服务器发送的信息
    char        buf[MAXLINE];
    std::string msg = "abcdefg" + std::string("(") + localIP + std::string(")");
    while (1) {
        //发送数据
        send(tcpClientSocket, msg.c_str(), msg.length(), 0);
        printf("[%s] send to server: %s\n", __func__, msg.c_str());

        //接收服务器返回的数据
        int n = recv(tcpClientSocket, buf, MAXLINE, MSG_DONTWAIT); //非阻塞读取
        if (n > 0) {
            printf("[%s] Response from server: %s\n", __func__, buf);
        }

        sleep(2);
    }
    //关闭连接
    close(tcpClientSocket);
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

int main(void)
{
    bool   bHasGetServerIP = false;
    thread th_tcp_client;

    std::string localIP = "xxx";
    if (true == get_local_ip(localIP)) {
        printf("[%s] localIP: [%s] %s\n", __func__, ETH_NAME, localIP.c_str());
    }

    int udpClientSocket = -1;
    if ((udpClientSocket = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        printf("[%s] socket error\n", __func__);
        return false;
    }

    struct sockaddr_in udpClientAddr;
    memset(&udpClientAddr, 0, sizeof(struct sockaddr_in));
    udpClientAddr.sin_family      = AF_INET;
    udpClientAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    udpClientAddr.sin_port        = htons(6000);
    int nlen                      = sizeof(udpClientAddr);

    set_sockopt_broadcast(udpClientSocket);

    json   js_send_msg;
    json   js_recv_msg;
    string smsg;

    while (1) {
        sleep(1);
        if (bHasGetServerIP) {
            continue; //获取到服务器的IP后, 就不需要再广播了
        }

        js_send_msg.clear();
        js_recv_msg.clear();
        smsg.clear();
        //从广播地址发送消息
        // std::string smsg = REQUEST_INFO + INFO_SPLIT + localIP;
        js_send_msg["type"]          = 0x01;
        js_send_msg["from"]          = "client";
        js_send_msg["content"]["ip"] = localIP;
        smsg                         = js_send_msg.dump();
        int ret                      = sendto(
            udpClientSocket, smsg.c_str(), smsg.length(), 0, (sockaddr *)&udpClientAddr, nlen);
        if (ret < 0) {
            printf("[%s] sendto error, ret: %d\n", __func__, ret);
        } else {
            printf("[%s] broadcast ok, msg: %s\n", __func__, smsg.c_str());

            /* 设置阻塞超时 */
            struct timeval timeOut;
            timeOut.tv_sec  = 2; //设置2s超时
            timeOut.tv_usec = 0;
            if (setsockopt(udpClientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeOut, sizeof(timeOut)) <
                0) {
                printf("[%s] time out setting failed\n", __func__);
                return 0;
            }

            //再接收数据
            char recvbuf[256] = { 0 };
            int  num          = recvfrom(udpClientSocket,
                               recvbuf,
                               256,
                               0,
                               (struct sockaddr *)&udpClientAddr,
                               (socklen_t *)&nlen);
            if (num > 0) {
                printf("[%s] receive server reply:%s\n", __func__, recvbuf);
                //解析服务器的ip
                if (!json::accept(string(recvbuf))) {
                    printf("Can't not converse to json.\n");
                    continue;
                }
                js_recv_msg = json::parse(recvbuf);
                try {
                    string serverIP;
                    js_recv_msg["content"]["server_ip"].get_to(serverIP);
                    bHasGetServerIP      = true;
                    th_tcp_client        = thread(tcp_client_thread, serverIP, localIP);
                    th_tcp_client.join();

                } catch (const std::exception &e) {
                    printf("can't not parse json content");
                }
            } else if (num == -1 && errno == EAGAIN) {
                printf("[%s] receive timeout\n", __func__);
            }
        }
    }

    return 0;
}