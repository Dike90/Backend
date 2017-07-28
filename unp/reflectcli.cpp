#include<iostream>
#include<cstring>
#include<cerrno>
#include<cstdio>
#include<sys/socket.h>
#include<sys/types.h>
#include <netinet/in.h>
#include<unistd.h>
#include<stdlib.h>
#include<arpa/inet.h>

using namespace std;
void str_cli(int );
int main(int argc ,char **argv){
    int sockfd;
	struct sockaddr_in servaddr;
    sockfd = socket(AF_INET,SOCK_STREAM,0);
	memset(&servaddr,0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");
	servaddr.sin_port = htons(10188);
	//bind(listenfd,(sockaddr*)&servaddr,sizeof(servaddr));
	//listen(listenfd,1024);
	int ret = connect(sockfd,(sockaddr*)&servaddr,sizeof(servaddr));
	if (ret == -1)
        perror("connect error!");
    str_cli(sockfd);

    return 0;
}

void str_cli(int sockfd){
    char buf[1024];
    cin.get(buf,1024,'\n');
    int len = cin.gcount();
    write(sockfd,buf,len);
    read(sockfd,buf,1024);
    cout<<buf;
}
