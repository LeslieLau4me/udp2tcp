#include "common.h"
#define SERVER_IP_ADDR "0.0.0.0"
#define SERVER_PORT    19999
#define BUFFER_SIZE    100
#define MAX_CONN_NUM   10

/*
	tcp server
	双向通信：客户端发送信息给服务器
*/

//清除缓冲区
void safe_flush(FILE *fp)
{
    int ch;
    while ((ch = fgetc(fp)) != EOF && ch != '\n')
        ;
}

void *send_msg(void *arg)
{
    int *pFd              = (int *)arg;
    char buf[BUFFER_SIZE] = { 0 };
    while (1) {
        bzero(buf, BUFFER_SIZE);
        printf("input msg for send to client\n");
        scanf("%s", buf);
        safe_flush(stdin);
        write(*pFd, buf, strlen(buf));
    }
}

int main(void)
{
    int  server_fd = -1;
    int  conn_fd   = -1;
    int  ret       = -1;
    char buf[BUFFER_SIZE];
    //定义ipv4地址结构体变量
    struct sockaddr_in bindAddr;
    bzero(buf, BUFFER_SIZE);
    bzero(&bindAddr, sizeof(bindAddr));
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = inet_addr(SERVER_IP_ADDR); //绑定服务器自己的ip地址
    bindAddr.sin_port        = htons(SERVER_PORT);        //绑定服务器自己的端口号

    struct sockaddr_in clientAddr;
    bzero(&clientAddr, sizeof(clientAddr));
    int addrSize = sizeof(clientAddr);

    //创建tcp套接字
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("创建tcp套接字!\n");
        return -1;
    }

    //设置端口重复使用
    int on = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    printf("旧的文件描述符:%d\n", server_fd);
    //绑定ip和端口号
    ret = bind(server_fd, (struct sockaddr *)&bindAddr, sizeof(bindAddr));
    if (ret == -1) {
        perror("绑定失败！\n");
        return -1;
    }
    //监听
    ret = listen(server_fd, MAX_CONN_NUM);
    if (ret == -1) {
        perror("监听失败！\n");
        return -1;
    }
    //接收客户端的连接请求
    conn_fd = accept(server_fd, (struct sockaddr *)&clientAddr, &addrSize);
    if (conn_fd == -1) {
        perror("接收客户端的连接请求失败!\n");
        return -1;
    }
    printf("新的文件描述符:%d\n", conn_fd);
    pthread_t tid;
    pthread_create(&tid, NULL, send_msg, &conn_fd);
    //接收信息
    while (1) {
        bzero(buf, BUFFER_SIZE);
        ret = read(conn_fd, buf, BUFFER_SIZE);
        if (ret == 0) {
            break;
        }
        //ret = read(server_fd, buf, BUFFER_SIZE);  //错误示范，用错了文件描述符
        printf("read的返回值是:%d\n", ret);
        printf("客户端发送给我的信息是:%s\n", buf);
    }
}
