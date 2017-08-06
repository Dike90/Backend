#ifndef __HTTP_COON_H__
#define __HTTP_COON_H__

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cassert>
#include <sys/stat.h>
#include <cstring>
#include <pthread.h>
#include <cstdio>
#include <sys/mman.h>
#include <stdarg.h>
#include <cerrno>
#include "locker.h"

#define READ 1
#define WRITE 2

class http_conn {
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD {GET=0 , POST , HEAD, PUT , DELETE , TRACE , OPTION , CONNECT , PATCH};
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER , CHECK_STATE_CONTENT };
    enum HTTP_CODE { NO_REQUEST , GET_REQUEST , BAD_REQUEST , NO_RESOURCE , FORBIDDEN_REQUEST ,FILE_REQUEST ,INTERAL_ERROR ,CLOSED_CONNECTION};
    enum LINE_STATUS {LINE_OK=0 , LINE_BAD , LINE_OPEN};
public:
    http_conn() {}
    ~http_conn(){}
public:
    void init (int sockfd , const sockaddr_in& addr);
    void close_conn(bool real_close = true);
    void process();
    bool read();
    bool write();

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char*);
    HTTP_CODE parse_headers(char*);
    HTTP_CODE parse_content(char*);
    HTTP_CODE do_request();
    char* get_line() { return m_read_buf + m_start_line;}
    LINE_STATUS parse_line();

    void unmap();
    bool add_response(const char* format,...);
    bool add_content(const char* content);
    bool add_status_line (int , const char*);
    bool add_headers(int );
    bool add_content_length(int );
    bool add_linger();
    bool add_blank_line();

public:
    //所有socket上的事件都被注册到同一个epoll内核事件表中
    static int m_epollfd;
    //统计用户数量
    static int m_user_count;

    int m_state;

private:
    //该HTTP连接的socket
    int m_sockfd;
    //对方的socket地址
    sockaddr_in m_address;
    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //标识已读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
    //当前正在分析的字符在读缓冲区中的位置
    int m_checked_idx;
    //当前正在解析的行起始位置
    int m_start_line;
    //写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    //写缓冲区中待发送字节的个数
    int m_write_idx;

    //主状态机所处的状态
    CHECK_STATE m_checked_state;
    //请求方法
    METHOD m_method;

    //客户请求的目标文件的完整路径，等于doc_root +_m_url的内容
    char m_real_file[FILENAME_LEN];
    //客户请求的url
    char* m_url;
    //HTTP协议号
    char* m_version;
    //主机名
    char* m_host;
    //HTTP请求的消息体长度
    int m_content_length;
    //请求是否要求保持连接
    bool m_linger;

    //客户请求的目标文件被mmap到内存中的起始位置
    char* m_file_address;
    //目标文件的状态，通过它我们可以判断文件是否存在，是否为目录，是否可读，并获取文件大小等
    struct stat m_file_stat;
    //我们采用writev来执行写操作
    struct iovec m_iv[2];
    //被写内存块的数量
    int m_iv_count;


};


#endif // __HTTP_COON_H__
