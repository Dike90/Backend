#include<iostream>
#include<cstring>
#include<sys/socket.h>
#include<sys/types.h>
#include <netinet/in.h>
#include<unistd.h>
#include<stdlib.h>

using namespace std;
void str_echo(int);
int main(int argc ,char **argv){
    int listenfd,connfd;
	pid_t childpid;
	socklen_t clilen;
	struct sockaddr_in cliaddr, servaddr;

	listenfd = socket(AF_INET , SOCK_STREAM, 0);
	memset(&servaddr,0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port = htons(10188);
	bind(listenfd,(sockaddr*)&servaddr,sizeof(servaddr));
	listen(listenfd,1024);
	for (;;){
        clilen = sizeof(cliaddr);
        connfd = accept(listenfd,(sockaddr*)&cliaddr,&clilen);
        if ( (childpid = fork())== 0 ){
            close(listenfd);
            str_echo(connfd);
            exit(0);
        }
        close(connfd);
	}
}
void str_echo(int connfd){
    char buf[1024];
    int n = read(connfd,buf,1024);
    std::cout<<buf<<std::endl;
    write(connfd,buf,n);
}

