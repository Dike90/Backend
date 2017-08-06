#include <iostream>
#include "http_conn.h"

using namespace std;

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
const char* doc_root = "/home/dk/www";

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    //新添加的sockfd的注册事件：有数据可读，对端关闭，并把通知方式设置为了边沿触发。
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    //如果只需要通知一次，就设置EPOLLONESHOT
    if (one_shot){
        event.events |= EPOLLONESHOT;
    }
    //将新的以连接socket描述符添加到epoll实例的兴趣列表
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd , &event);
    //设置为非阻塞I/O
    setnonblocking(fd);
}
//将文件描述符fd从epoll的兴趣列表中移除。并关闭这个文件描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL ,fd ,0);
    close(fd);
}

void modfd(int epollfd , int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    //添加感兴趣事件。并设置为边沿触发，触发一次；和对端关闭事件
    event.events = ev| EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD , fd ,&event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn( bool real_close)
{
    if (real_close && (m_sockfd != -1)){
        removefd(m_epollfd , m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd , const sockaddr_in& addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse = -1;
    //setsockopt(m_sockfd , SOL_SOCKET , SO_REUSEADDR , &reuse , sizeof(reuse));
    //注册该socket描述符的读事件，并设置为边沿触发，只触发一次。
    addfd(m_epollfd,sockfd , true);
    m_user_count++;

    init();
}
//类的初始状态。
void http_conn::init()
{
    //检查状态为：检查请求行
    m_checked_state = CHECK_STATE_REQUESTLINE;
    //keep-alive为假
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf,0 , READ_BUFFER_SIZE);
    memset(m_write_buf , 0 , WRITE_BUFFER_SIZE);
    memset(m_real_file,0 , FILENAME_LEN);

}
//从状态机，用于解析出一行内容,其实就是判断是否读到了一个完整的行
//如果读到一个完整的行就返回LINE_OK,如果不完整就返回LINE_OPEN，如果不是以CRLF结尾就返回LINE_BAD
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //m_checked_idx指向读缓冲区中当前正在分析的字节m_read_idx指向读缓冲区的尾后字节
    //读缓冲区中0~m_checked_idx已经分析完毕，m_checked_idx~(m_read_idx-1)由下面的循环分析
    for (; m_checked_idx < m_read_idx ; ++m_checked_idx){
        temp = m_read_buf[m_checked_idx]; //获得当前要分析的字节
        //如果当前字节是CR，则说明可能读到了一个完整的行
        if ( temp == '\r'){
            //如果CR是当前buffer中最后一个字节，那么这次分析没有读到一个完整的行，返回LIN_OPEN，需要继续读取客户数据进一步分析
            if ( (m_checked_idx +1)==m_read_idx)
                return LINE_OPEN;
            //如果CR字符的下一个字符是LF，那么说明已经读到了一个完成的行，返回LINE_OK,并且将CRLF替换成'\0\0';
            else if ( m_read_buf[m_checked_idx+1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则的话说明请求数据有问题，返回LINE_BAD
            return LINE_BAD;
        }
        //如果当前字节是LF,则分析LF之前的字符是否是CR
        else if ( temp == '\n'){
            //如果LF之前有至少一个字符，并且这个字符是CR，则说明读到了一个完整的行，返回LINE_OK
            if ( (m_checked_idx>1) && (m_read_buf[m_checked_idx-1]) =='\r'){
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则返回LINE_BAD
            return LINE_BAD;
        }
    }
    //如果解析完还没发现CR LF就表示还需要进一步的读取数据
    return LINE_OPEN;
}
//循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if ( m_read_idx >= READ_BUFFER_SIZE)
        return false;
    //每次调用recv读取的字节数
    int bytes_read = 0;
    while ( true)
    {
        bytes_read = recv(m_sockfd , m_read_buf+m_read_idx , READ_BUFFER_SIZE - m_read_idx , 0);
        if (bytes_read == -1){
            //如果读取将会阻塞就返回true，等待下次事件触发读取
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            //如果是其他错误就返回false
            return false;
        }
        //如果读出的数据等于0，说明对端已经关闭了连接，也返回false
        //或者缓冲区已经满了
        else if ( bytes_read == 0){
            return false;
        }
        //更新已读取字节数
        m_read_idx += bytes_read;
    }
    return true;


}
//分析请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //如果请求行中没有空格，则请求行必有问题
    m_url = strpbrk(text, " ");
    if (!m_url)
        return BAD_REQUEST;
    //将空格替换成'\0',并且当前m_url指向下一个待处理的字符
    *m_url++ = '\0';
    //提取method字段,这里仅支持GET方法
    char* method = text;
    if ( strcasecmp(method , "GET") == 0){
        m_method = GET;
    }
    else{
        return BAD_REQUEST;
    }
    //当前m_url指向的是url字段
    //处理多余的空格符，执行后，m_url指向url字段第一个非空白字符
    m_url += strspn(m_url , " ");
    //在url字段后提取version,m_version指向url字段后的第一个空白符
    m_version = strpbrk(m_url," ");
    if (!m_version)
        return BAD_REQUEST;
    //将url字段后的空白符替换为'\0'，m_version指向下一个字节
    *m_version++ = '\0';
    //处理多余的空白符
    m_version += strspn(m_version ," ");
    //判断是否是1.1版本，这里仅支持1.1版本
    if ( strcasecmp(m_version , "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }
    //如果url带http://，则处理掉
    if (strncasecmp(m_url ,"http://",7) == 0){
        m_url += 7;
        m_url = strchr(m_url , '/');
    }
    //如果url字段第一个字符不是'/'则说明请求有误
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    cout<<"the request URL is :"<<m_url<<endl;
    //http请求行处理完毕，状态转移到头部分析
    m_checked_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}
//分析请求头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    //执行parse_line后，会将CRLF替换成'\0''\0'
    //如果遇到空行，表示头部字段解析完毕
    if (text[0] == '\0'){
        //如果请求有消息体，还需要读取消息体，进入CHECK_STATE_CONTENT状态
        if (m_content_length !=0){
            m_checked_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        cout<<"get request"<<endl;
        return GET_REQUEST;
    }
    //处理Connection头部字段
    else if (strncasecmp(text , "Connection:",11) == 0){
        text += 11;
        text += strspn(text, " ");
        if ( strcasecmp(text, "keep-alive")==0){
            m_linger = true;
        }
    }
    //处理content-length字段
    else if (strncasecmp(text, "Content-Length:",15) == 0){
        text += 15;
        text += strspn(text, " ");
        m_content_length = atol(text);
    }
    //处理host字段
    else if (strncasecmp(text, "Host:",5) == 0){
        text += 5;
        text += strspn(text, " ");
        m_host = text;
    }
    //其他字段不做处理
    else {
        cout<< "don't care: "<<text<<endl;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
//主状态机，已经读取的客户数据
http_conn::HTTP_CODE http_conn::process_read()
{
    //记录当前行的读取状态
    LINE_STATUS line_status = LINE_OK;
    //记录HTTP请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char* text =0;
    //初始状态，m_checked_state=CHECK_STATE_REQUESTLINE;
    //parse_line函数用来分析，当前请求数据是否是一个完整的行
    while( ( (m_checked_state == CHECK_STATE_CONTENT)&&(line_status==LINE_OK)) ||((line_status = parse_line() ) == LINE_OK)){
        //处理新的一行，text = m_read_buf + m_start_line;
        text = get_line();
        //记录下一行的起始位置
        m_start_line = m_checked_idx;
        cout<<"handled one line:"<<text<<endl;
        //m_checked_state记录主状态机的当前状态
        switch (m_checked_state)
        {
        //第一个状态。分析请求行
        case CHECK_STATE_REQUESTLINE:
            {
                //parse_request_line中，将主状态机更新至CHECK_STATE_HEADER状态
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }

        //第二个状态。分析头部
        case CHECK_STATE_HEADER:
            {
                //parse_headers()中，如果存在content-length就将主状态机更新至CHECK_STATE_CONTENT状态
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST)
                    return do_request();
                break;
            }

        case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();
                line_status = LINE_OPEN;
                break;
            }
        default:
            {
                return INTERAL_ERROR;
            }

        }
    }
    return NO_REQUEST;
}

//当得到一个完整的,正确的HTTP请求。我们就开始分析客户所请求的目标文件的属性。
//如果目标文件存在，并对所有用户可读，且不是一个目录，就使用mmap将其映射到m_file_address处。并告诉调用者，获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    //将根目录字符复制到m_real_file处
    strcpy(m_real_file , doc_root);
    int len = strlen(doc_root);
    //将请求的url与根目录组合在一起，形成一个完整的目录
    strncpy(m_real_file + len , m_url , FILENAME_LEN-len -1);
    //调用系统调用stat获取文件的属性，存储在m_file_stat中
    cout<<"the request file is :"<<m_real_file<<endl;
    if (stat(m_real_file ,&m_file_stat) < 0)
        return NO_RESOURCE;
    //如果文件不是组可读就返回FORBIDDEN_REQUEST
    if ( !(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    //如果请求的文件是一个目录，就返回BAD_REQUEST
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    //以只读方式打开文件
    int fd = open(m_real_file ,O_RDONLY);
    //对打开的文件以私有模式进行映射。
    m_file_address = (char*) mmap(NULL, m_file_stat.st_size , PROT_READ , MAP_PRIVATE ,fd , 0);
    //关闭文件并不会对映射产生影响。
    close(fd);
    //说明请求的文件已经映射成功。
    //cout<<"File is ready"<<endl;
    return FILE_REQUEST;
}
//对映射区执行munmap操作。
void http_conn::unmap()
{
    if (m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address = 0;
    }
}
//写HTTP响应
bool http_conn::write()
{
    //cout<<"I am writting"<<endl;
    int temp = 0;
    int bytes_have_send = 0;
    //int bytes_to_send = m_write_idx ;
    int bytes_to_send = m_write_idx + m_file_stat.st_size;
    //如果没没要发送的数据就重新将该socket的感兴趣事件设置为读事件
    if (bytes_to_send == 0){
        modfd(m_epollfd , m_sockfd , EPOLLIN);
        //重新初始化
        init();
        return true;
    }
    //有数据要发就发送所有需要发送的数据
    while (1){
        //集中写
        temp = writev( m_sockfd , m_iv , m_iv_count);
        if (temp <= -1){
            //如果写事件会阻塞就再次注册可写事件，并返回，等待下次事件通知
            if (errno == EAGAIN){
                modfd(m_epollfd , m_sockfd , EPOLLOUT);
                return true;
            }
            //其他错误就取消目标文件的内存映射并返回false
            unmap();
            return false;
        }
        //更新待发送字节数
        bytes_to_send -= temp;
        //更新已发送字节数
        bytes_have_send += temp;
        //if (bytes_to_send <= bytes_have_send) {
        if (bytes_to_send <= 0){
            unmap();
            if (m_linger){
                init();
                modfd(m_epollfd , m_sockfd , EPOLLIN);
                return true;
            }
            else {
                modfd(m_epollfd ,m_sockfd , EPOLLIN);
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char* format , ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list , format);
    int len = vsnprintf(m_write_buf + m_write_idx , WRITE_BUFFER_SIZE -1 - m_write_idx , format ,arg_list);
    if (len >= (WRITE_BUFFER_SIZE -1 - m_write_idx))
        return false;
    m_write_idx += len ;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status , const char* title)
{
    return add_response("%s %d %s\r\n" , "HTTP/1.1" , status , title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true)? "keep-alive":"close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

bool http_conn::add_content(const char* content)
{
    return add_response("%s" , content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
    case INTERAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }

    case BAD_REQUEST:
        {
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form))
                return false;
            break;
        }

    case NO_RESOURCE:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }

    case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }

    case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size !=0){
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count =2;
                cout<<"What I want write is :"<<m_write_buf<<endl;
                return true;
            }
            else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
    default:
        {
            return false;
        }
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    //调用process_read()来分析请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST){
        modfd(m_epollfd , m_sockfd , EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret){
        close_conn();
    }
    modfd(m_epollfd , m_sockfd , EPOLLOUT);
}



