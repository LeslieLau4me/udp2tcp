#include "common.h"
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <stdio.h>
#include <string>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#define TCP_PORT     19999
#define MAXLINE      1024
#define MAXSIZE      1024
#define EPOLLEVENTS  100
#define FDSIZE       20
#define UDP_TIME_OUT 3
#define UDP_PORT     6000
#define MAX_TCP_CONN 20
#define MAX_UDP_RECV 1024

using namespace std;
using json = nlohmann::json;
#define ETH_NAME     "enp3s0"
#define REQUEST_INFO "new_client_ip" //客户端发送的广播信息头
#define REPLAY_INFO  "server_ip"     //服务端回复的信息头
#define JS_MSG_TYPE  "type"
#define JS_MSG_CONT  "content"
#define INFO_SPLIT   std::string(":") //信息分割符

void tcp_loop_thread(void);

void tcp_client_thread_start(void);

double difftimespec(struct timespec end, struct timespec beginning)
{
    double x;

    x = 1E-9 * (double)(end.tv_nsec - beginning.tv_nsec);
    x += (double)(end.tv_sec - beginning.tv_sec);

    return x;
}

enum workMode {
    PKFD = 0x00,
    BAST,
    BRDGE,
};

enum msgType {
    CHECK_ONLINE = 0x00,
    CLIENT_REQ_IP,
    RETURN_IP,
    RETURN_ONLINE,
};

class ProtocolHandler
{
  private:
    /* data */
    string local_ip;
    string tcp_server_ip;
    int    tcp_fd       = -1;
    int    epoll_fd     = -1;
    int    work_mode    = -1;
    bool   to_be_server = false;

    struct sockaddr_in tcp_server_addr;
    struct epoll_event events[EPOLLEVENTS];

    bool epoll_set_fd_a_event(int opt, int fd, int event);
    void tcp_server_run(void);

    int                udp_fd = -1;
    struct sockaddr_in udp_addr;

    void udp_msg_json_handle(int type, json &json_obj);
    void udp_server_loop_start(void);
    void udp_client_loop_start(void);

    void init_tcp_server_socket(void);
    void init_tcp_client_socket(void);

    void init_udp_server_socket(void);
    void init_udp_client_socket(void);

    void tcp_client_run(void);

  public:
    ProtocolHandler(/* args */);
    ~ProtocolHandler();
    void init_tcp_socket(void);
    void init_udp_socket(void);
    void tcp_loop_run(void);
    void clean_all(void);
    void udp_loop_run(void);
    void udp_online_check(void);
    void get_local_ip_string(void);
};

static ProtocolHandler thrd;

static void sig_handler(int sigio)
{
    printf("Receive signo :[%d].\n", sigio);
    if (sigio == SIGQUIT) {
        thrd.clean_all();
        exit(0);
    } else if ((sigio == SIGINT) || (sigio == SIGTERM)) {
        // 预留操作空间
        thrd.clean_all();
        exit(0);
    }
}

ProtocolHandler ::ProtocolHandler(/* args */) {}

ProtocolHandler ::~ProtocolHandler()
{
    close(this->epoll_fd);
    close(this->tcp_fd);
    close(this->udp_fd);
}

void ProtocolHandler ::clean_all(void)
{
    close(this->epoll_fd);
    close(this->tcp_fd);
    close(this->udp_fd);
}

//获取本机ip(根据实际情况修改ETH_NAME)
void ProtocolHandler ::get_local_ip_string(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        printf("[%s] socket err!\n", __func__);
        exit(-1);
    }

    struct ifreq ifr;
    bzero(&ifr, sizeof(struct ifreq));
    memcpy(&ifr.ifr_name, ETH_NAME, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = 0;
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        printf("[%s] ioctl err!\n", __func__);
        close(fd);
        exit(-1);
    }
    struct sockaddr_in sin;
    memcpy(&sin, &ifr.ifr_addr, sizeof(sin));
    this->local_ip = std::string(inet_ntoa(sin.sin_addr));
    printf("[%s]Successfully get local ip:[%s]. \n", __func__, this->local_ip.c_str());
    close(fd);
    return;
}

void ProtocolHandler::udp_msg_json_handle(int type, json &json_obj)
{
    json_obj["from"] = "server";
    if (type == CLIENT_REQ_IP) {
        json_obj["content"]["server_ip"] = this->local_ip;
        json_obj["type"]                 = RETURN_IP;
    } else if (type == CHECK_ONLINE) {
        json_obj["content"]["online"] = true;
        json_obj["type"]              = RETURN_ONLINE;
    }
}

void ProtocolHandler::tcp_client_run(void)
{
    //要向服务器发送的信息
    char        buf[MAXLINE];
    std::string msg = "abcdefg" + std::string("(") + this->local_ip + std::string(")");
    while (1) {
        //发送数据
        send(this->tcp_fd, msg.c_str(), msg.length(), 0);
        printf("[%s] send to server: %s\n", __func__, msg.c_str());

        //接收服务器返回的数据
        int n = recv(this->tcp_fd, buf, MAXLINE, MSG_DONTWAIT); //非阻塞读取
        if (n > 0) {
            printf("[%s] Response from server: %s\n", __func__, buf);
        }
        sleep(2);
    }
}

void ProtocolHandler::init_udp_server_socket(void)
{
    if ((this->udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        printf("[%s] socket error\n", __func__);
        return;
    }
    bzero(&this->udp_addr, sizeof(struct sockaddr_in));
    this->udp_addr.sin_family      = AF_INET;
    this->udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    this->udp_addr.sin_port        = htons(UDP_PORT);
    if (bind(this->udp_fd, (struct sockaddr *)&(udp_addr), sizeof(struct sockaddr_in)) == -1) {
        printf("[%s] bind error\n", __func__);
        return;
    }
    // 设置广播
    const int opt = 1;
    int       nb  = setsockopt(this->udp_fd, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
    if (nb == -1) {
        printf("[%s] set socket error\n", __func__);
        return;
    }
}

void ProtocolHandler::init_udp_client_socket(void)
{
    if ((this->udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        printf("[%s] socket error\n", __func__);
        return;
    }
    bzero(&this->udp_addr, sizeof(struct sockaddr_in));
    this->udp_addr.sin_family      = AF_INET;
    this->udp_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    this->udp_addr.sin_port        = htons(UDP_PORT);
    // 设置广播
    const int opt = 1;
    int       nb  = setsockopt(this->udp_fd, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
    if (nb == -1) {
        printf("[%s] set socket error\n", __func__);
        return;
    }
}

void ProtocolHandler::udp_client_loop_start(void)
{
    bool   b_has_got_server_ip = false;
    thread th_tcp_client;
    json   js_send_msg;
    json   js_recv_msg;
    string smsg;
    char   recvbuf[MAX_UDP_RECV] = { 0 };
    int    nlen                  = sizeof(this->udp_addr);
    while (1) {
        sleep(1);
        if (b_has_got_server_ip) {
            continue; //获取到服务器的IP后, 就不需要再广播了
        }

        js_send_msg.clear();
        js_recv_msg.clear();
        smsg.clear();
        //从广播地址发送消息
        // std::string smsg = REQUEST_INFO + INFO_SPLIT + localIP;
        js_send_msg["type"]          = 0x01;
        js_send_msg["from"]          = "client";
        js_send_msg["content"]["ip"] = this->local_ip;
        smsg                         = js_send_msg.dump();

        int ret =
            sendto(this->udp_fd, smsg.c_str(), smsg.length(), 0, (sockaddr *)&this->udp_addr, nlen);
        if (ret < 0) {
            printf("[%s] sendto error, ret: %d\n", __func__, ret);
        } else {
            printf("[%s] broadcast ok, msg: %s\n", __func__, smsg.c_str());

            /* 设置阻塞超时 */
            struct timeval timeOut;
            timeOut.tv_sec  = 2; //设置2s超时
            timeOut.tv_usec = 0;
            if (setsockopt(this->udp_fd, SOL_SOCKET, SO_RCVTIMEO, &timeOut, sizeof(timeOut)) < 0) {
                printf("[%s] time out setting failed\n", __func__);
                return;
            }

            //再接收数据
            bzero(recvbuf, MAX_UDP_RECV);
            int num = recvfrom(this->udp_fd,
                               recvbuf,
                               MAX_UDP_RECV,
                               0,
                               (struct sockaddr *)&this->udp_addr,
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
                    js_recv_msg["content"]["server_ip"].get_to(this->tcp_server_ip);
                    b_has_got_server_ip = true;
                    // 初始化tcp socket 开启udp 客户端线程
                    this->init_tcp_client_socket();
                    th_tcp_client = thread(tcp_loop_thread);
                    th_tcp_client.join();

                } catch (const std::exception &e) {
                    printf("can't not parse json content");
                }
            } else if (num == -1 && errno == EAGAIN) {
                printf("[%s] receive timeout\n", __func__);
            }
        }
    }
}

void ProtocolHandler::udp_server_loop_start(void)
{
    int    ret                   = -1;
    char   rev_msg[MAX_UDP_RECV] = { 0 };
    int    len                   = sizeof(sockaddr_in);
    json   js_send_msg;
    json   js_recv_msg;
    string client_ip;
    string reply_content;
    int    opr_type = -1;
    while (1) {
        memset(&rev_msg, 0, MAX_UDP_RECV);
        js_recv_msg.clear();
        js_send_msg.clear();
        reply_content.clear();
        //从广播地址接收消息
        ret = recvfrom(this->udp_fd,
                       rev_msg,
                       MAX_UDP_RECV,
                       0,
                       (struct sockaddr *)&udp_addr,
                       (socklen_t *)&len);
        if (ret <= 0) {
            printf("[%s] read error, ret:%d\n", __func__, ret);
        } else {
            printf("[%s]receive: %s\n", __func__, rev_msg);
            if (!json::accept(string(rev_msg))) {
                printf("Can't no converse to json. \n");
                continue;
            }
            js_recv_msg = json::parse(rev_msg);
            try {
                js_recv_msg["type"].get_to(opr_type);
                if (opr_type != CHECK_ONLINE) {
                    js_recv_msg["content"]["ip"].get_to(client_ip);
                }
                printf("client ip is :%s \n", client_ip.c_str());
            } catch (const std::exception &e) {
                printf("Error, no such option js node .\n");
                continue;
            }
            // 处理返回的填写json对象
            this->udp_msg_json_handle(opr_type, js_send_msg);
            reply_content = js_send_msg.dump();
            // 广播出去
            ret = sendto(this->udp_fd,
                         reply_content.c_str(),
                         reply_content.length(),
                         0,
                         (struct sockaddr *)&udp_addr,
                         len);
            if (ret < 0) {
                printf("[%s] sendto error, ret: %d\n", __func__, ret);
            } else {
                printf("[%s] reply ok, msg: %s\n", __func__, reply_content.c_str());
            }
        }

        sleep(1);
    }
}

void ProtocolHandler::init_udp_socket(void)
{
    if (this->to_be_server) {
        this->init_udp_server_socket();
    } else {
        this->init_udp_client_socket();
    }
}

//为epoll中的某个fd添加/修改/删除某个事件
bool ProtocolHandler::epoll_set_fd_a_event(int opt, int fd, int event)
{
    if (EPOLL_CTL_ADD == opt || EPOLL_CTL_MOD == opt || EPOLL_CTL_DEL == opt) {
        struct epoll_event ev;
        ev.events  = event;
        ev.data.fd = fd;
        epoll_ctl(this->epoll_fd, opt, fd, &ev);
        return true;
    } else {
        printf("[%s] err op:%d\n", __func__, opt);
        return false;
    }
}

void ProtocolHandler::tcp_server_run(void)
{
    char buf[MAXSIZE];
    int  num = 0;
    memset(&buf, 0, MAXSIZE);
    this->epoll_fd = epoll_create(FDSIZE);
    printf("[%s] create epoll_fd:%d\n", __func__, this->epoll_fd);
    //添加监听描述符事件
    this->epoll_set_fd_a_event(EPOLL_CTL_ADD, this->tcp_fd, EPOLLIN);
    while (1) {
        //获取已经准备好的描述符事件
        printf("[%s] epoll_fd:%d epoll_wait...\n", __func__, epoll_fd);
        num = epoll_wait(this->epoll_fd, this->events, EPOLLEVENTS, -1);
        for (int i = 0; i < num; i++) {
            int fd = this->events[i].data.fd;
            //tcp_fd说明有新的客户端请求连接
            if ((fd == this->tcp_fd) && (this->events[i].events & EPOLLIN)) {
                //accept客户端的请求
                struct sockaddr_in cliaddr;
                socklen_t          cliaddrlen = sizeof(cliaddr);
                int clifd = accept(this->tcp_fd, (struct sockaddr *)&cliaddr, &cliaddrlen);
                if (clifd == -1) {
                    perror("accpet error:");
                } else {
                    printf("[%s] accept a new client(fd:%d): %s:%d\n",
                           __func__,
                           clifd,
                           inet_ntoa(cliaddr.sin_addr),
                           cliaddr.sin_port);
                    //将客户端fd添加到epoll进行监听
                    this->epoll_set_fd_a_event(EPOLL_CTL_ADD, clifd, EPOLLIN);
                }
            }
            //收到已连接的客户端fd的消息
            else if (this->events[i].events & EPOLLIN) {
                memset(buf, 0, MAXSIZE);
                //读取客户端的消息
                int nread = read(fd, buf, MAXSIZE);
                if (nread == -1) {
                    perror("read error:");
                    close(fd);
                    this->epoll_set_fd_a_event(EPOLL_CTL_DEL, fd, EPOLLIN);
                } else if (nread == 0) {
                    printf("[%s] client(fd:%d) close.\n", __func__, fd);
                    close(fd);
                    this->epoll_set_fd_a_event(EPOLL_CTL_DEL, fd, EPOLLIN);
                } else {
                    //此处处理客户端返回的消息
                    printf("[%s] read message from fd:%d ---> %s\n", __func__, fd, buf);
                }
            }
        }
    }
}

void ProtocolHandler::init_tcp_server_socket(void)
{
    this->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&this->tcp_server_addr, sizeof(this->tcp_server_addr));
    this->tcp_server_addr.sin_family      = AF_INET;
    this->tcp_server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    this->tcp_server_addr.sin_port        = htons(TCP_PORT);
    //将套接字文件与服务器端口地址绑定
    bind(this->tcp_fd, (struct sockaddr *)&(this->tcp_server_addr), sizeof(this->tcp_server_addr));

    //监听，并设置最大连接数为20
    listen(this->tcp_fd, MAX_TCP_CONN);
    printf("[%s] Accepting connections... \n", __func__);
}

void ProtocolHandler::init_tcp_client_socket(void)
{
    printf("[%s] in, prepare connect serverIP:%s\n", __func__, this->tcp_server_ip.c_str());

    //创建客户端套接字文件
    this->tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    //初始化服务器端口地址
    bzero(&this->tcp_server_addr, sizeof(this->tcp_server_addr));
    this->tcp_server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, this->tcp_server_ip.c_str(), &this->tcp_server_addr.sin_addr);
    this->tcp_server_addr.sin_port = htons(TCP_PORT);

    //请求连接
    connect(
        this->tcp_fd, (struct sockaddr *)&(this->tcp_server_addr), sizeof(this->tcp_server_addr));
}

void ProtocolHandler::init_tcp_socket(void)
{
    if (this->to_be_server == true) {
        this->init_tcp_server_socket();
    } else {
        // do nothing 如果不是TYCP服务器端，在其他地方初始化socket
    }
}

void ProtocolHandler::udp_online_check(void)
{
    // 这个地方获取本地ip似乎不是太合理，构造函数里面会有Bug
    int udp_client_socket = -1;
    if ((udp_client_socket = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        printf("[%s] socket error\n", __func__);
        exit(-1);
    }

    struct sockaddr_in udpClientAddr;
    memset(&udpClientAddr, 0, sizeof(struct sockaddr_in));
    udpClientAddr.sin_family      = AF_INET;
    udpClientAddr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    udpClientAddr.sin_port        = htons(UDP_PORT);
    int nlen                      = sizeof(udpClientAddr);
    /* 设置阻塞超时 */
    struct timeval timeOut;
    timeOut.tv_sec  = 2; //设置2s超时
    timeOut.tv_usec = 0;
    const int opt   = 1;
    int nb = setsockopt(udp_client_socket, SOL_SOCKET, SO_BROADCAST, (char *)&opt, sizeof(opt));
    if (nb == -1) {
        printf("[%s] set socket error\n", __func__);
        exit(-1);
    }

    int  ret;
    json js_smsg;
    json js_rmsg;
    int  opr_type = -1;

    char            rev_msg[100] = { 0 };
    struct timespec send_time    = { 0 };
    struct timespec recv_time    = { 0 };

    js_smsg[JS_MSG_TYPE] = CHECK_ONLINE;
    js_smsg[JS_MSG_CONT] = "Are there other gateways online?";
    string js_string     = js_smsg.dump();
    printf("js_string is %s \n", js_string.c_str());
    // 发送一个json字符串，确认有无信息返回

    clock_gettime(CLOCK_MONOTONIC, &send_time);
    recv_time = send_time;
    while ((int)difftimespec(recv_time, send_time) < UDP_TIME_OUT) {
        // ret = send(udp_client_socket, js_string.c_str(), js_string.length(), 0);

        ret = sendto(udp_client_socket,
                     js_string.c_str(),
                     js_string.length(),
                     0,
                     (sockaddr *)&udpClientAddr,
                     nlen);
        if (ret < 0) {
            printf("[%s] sendto error, ret: %d\n", __func__, ret);
        } else {
            printf("[%s] broadcast ok, msg: %s\n", __func__, js_string.c_str());
        }

        if (setsockopt(udp_client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeOut, sizeof(timeOut)) < 0) {
            printf("[%s] time out setting failed\n", __func__);
            return;
        }
        // ret = recv(udp_client_socket, rev_msg, 100, MSG_DONTWAIT);

        ret = recvfrom(udp_client_socket,
                       rev_msg,
                       256,
                       0,
                       (struct sockaddr *)&udpClientAddr,
                       (socklen_t *)&nlen);
        clock_gettime(CLOCK_MONOTONIC, &recv_time);
        if (ret < 0) {
            continue;
        } else {
            if (!json::accept(string(rev_msg))) {
                printf("[%s]Can't no converse to json. \n", __func__);
                break;
            }
            js_rmsg = json::parse(rev_msg);
            try {
                js_rmsg["type"].get_to(opr_type);
                if (opr_type == RETURN_ONLINE) {
                    this->to_be_server = false;
                    close(udp_client_socket);
                    return;
                }
            } catch (const std::exception &e) {
                printf("Error, no such option js node .\n");
                break;
            }
        }
    }
    printf("[%s]check online time out, to be tcp server, ret:%d\n", __func__, ret);
    this->to_be_server = true;
    close(udp_client_socket);
}

void ProtocolHandler::tcp_loop_run(void)
{
    if (this->to_be_server == true) {
        this->tcp_server_run();
    } else {
        this->tcp_client_run();
    }
}

void ProtocolHandler::udp_loop_run(void)
{
    if (this->to_be_server == true) {
        this->udp_server_loop_start();
    } else {
        this->udp_client_loop_start();
    }
}

//TCP线程，视情况决定是客户端还是服务器
void tcp_loop_thread(void)
{
    thrd.init_tcp_socket();
    thrd.tcp_loop_run();
}

void udp_loop_thread(void)
{
    thrd.init_udp_socket();
    thrd.udp_loop_run();
}

void udp_check_onlie(void)
{
    thrd.udp_online_check();
}


int main(void)
{
    struct sigaction sigact; /* SIGQUIT&SIGINT&SIGTERM signal handling */
    /* configure signal handling */
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags   = 0;
    sigact.sa_handler = sig_handler;
    sigaction(SIGQUIT, &sigact, NULL); /* Ctrl-\ */
    sigaction(SIGINT, &sigact, NULL);  /* Ctrl-C */
    sigaction(SIGTERM, &sigact, NULL); /* default "kill" command */

    usleep(500);
    thrd.get_local_ip_string();
    thread th0(udp_check_onlie);
    th0.join();

    thread th1(udp_loop_thread);
    thread th2(tcp_loop_thread);
    th1.join();
    th2.join();

    return 0;
}