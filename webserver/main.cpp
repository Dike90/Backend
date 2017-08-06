#include <iostream>
#include"locker.h"
#include"threadpool.h"
#include "http_conn.h"
using namespace std;

#define MAX_FD 65535
#define MAX_EVENT_NUMBER 10000

extern int addfd(int epollfd , int fd , bool one_shot);
extern int removefd(int epollfd, int fd );

void addsig( int sig , void(handler)(int ) ,bool restart = true)
{
    struct sigaction sa ;
    memset(&sa , 0 , sizeof(sa));
    sa.sa_handler = handler;
    if (restart){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa , NULL) != -1);
}

void show_error( int connfd , const char* info)
{
    cout<<info<<endl;
    send(connfd , info , strlen(info), 0);
    close(connfd);
}

int main(int argc , char* argv[])
{
    if (argc <=2 ){
        cout <<"you shoud offer ip address and port";
        return -1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);
    //忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);
    //创建线程池
    threadpool<http_conn> * pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch(...)
    {
        return 1;
    }
    //预先为每个可能的客户连接分配一个http_conn对象
    http_conn * users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    int listenfd = socket(AF_INET, SOCK_STREAM , 0);
    assert(listenfd >= 0);
    struct linger tmp = {1,0};
    setsockopt(listenfd , SOL_SOCKET , SO_LINGER ,&tmp , sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip , &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind( listenfd, (sockaddr*)&address ,sizeof(address));
    assert(ret >=0);

    ret = listen(listenfd , 1024);
    epoll_event events[MAX_EVENT_NUMBER];
    //创建一个epoll实例
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    //将监听套接字加入兴趣列表，并设置EPOLLONESHOT = false;
    addfd(epollfd, listenfd , false);
    //将epoll实例的描述符传递给http_conn类的静态变量,所有http_conn对象将共享这个描述符。
    http_conn::m_epollfd = epollfd;

    while(true)
    {
        //无限阻塞，直到发生了一个感兴趣事件，或者被信号中断
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ( ( number < 0 )&& (errno != EINTR)){
            cout << "epoll failure"<<endl;
            break;
        }
        for ( int i = 0; i< number; ++i){
            //取出就绪状态的文件描述符
            int sockfd = events[i].data.fd;
    //如果就绪的文件描述符是监听套接字，就接受连接
            if( sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (sockaddr*)&client_address, &client_addrlength);
                if (connfd < 0){
                    cout << "errno is "<< errno<<endl;
                    continue;
                }
                //当前连接的客户端大于最大限制就关闭连接
                if (http_conn::m_user_count >= MAX_FD){
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                //对新连接的socket进行初始化，初始化的内容有：将此socket描述符添加到epoll的兴趣列表，并注册了EPOLLIN事件，设置为边沿触发，只触发一次。
                //http_conn类的初始化，各个变量初始化，http_conn对象的m_sockfd保存了该连接描述符，m_address保存了对端的地址,m_user_count加一
                users[connfd].init(connfd , client_address);
            }
    //如果不是监听套接字就检测就绪的事件是什么，并做相应的操作
            //如果是对端关闭或者有错误发生，就关闭该连接。
            else if ( events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                users[sockfd].close_conn();
            }
            //如果是可读事件，就进行读操作，并将解析任务添加到任务队列中
            else if (events[i].events & EPOLLIN){
                    users[sockfd].m_state =READ;
                    pool->append(users+ sockfd);
            }
            else if (events[i].events & EPOLLOUT){
                users[sockfd].m_state = WRITE;
                pool->append(users+sockfd);
            }
            else{

            }

        }
    }
    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;
}
