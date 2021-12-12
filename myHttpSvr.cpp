/*
 * myHttpSvr.cpp
 *
 *  Created on: 2021年12月12日
 *      Author: LENOVO
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/sendfile.h>

#define LISTEN_PORT 8888  //监听端口
#define PORT_COUNT	1

#define MAX_EPOLL_EVENTS 1024 //每块管理连接1024个客户端
#define MAX_EVENTBLOCKS 1 //最大块
#define BUFFER_LENGTH		4096

typedef  unsigned int uint;
typedef  unsigned short uint8;

typedef int NCALLBACK(int ,int, void*);
//每个连接的客户端
typedef struct EVENT
{
	int fd; //客户端fd
	int events; //事件
	void *arg; //参数
	int (*callback)(int fd,int events,void *arg);

	int status;
	char buffer[BUFFER_LENGTH];
	int length;
	long last_active;

	// http param
	int method; //
	char resource[BUFFER_LENGTH];
	int ret_code;
}event_t ;

//管理连接的客户端
typedef struct EVENTBLOCK
{
	struct EVENTBLOCK* next; //指向下一块
	struct EVENT* events; //每块里边有1024个客户端
}evblk_t;

typedef struct REACTOR
{
	int epfd; //epool
	int blkcnt; //1024为一块,记录有多少块
	struct EVENTBLOCK *evblk;//执向第一块
}reactor_t;

//函数声明
int reactor_init(reactor_t* re); //初始化reactor
int init_sock(uint8 port);	//初始化socket
int reactor_addlistener(reactor_t *re, int sockfd, NCALLBACK *acceptor);
int accept_cb(int fd, int events, void *arg);
int recv_cb(int fd, int events, void *arg);
int send_cb(int fd, int events, void *arg);
event_t *reactor_idx(reactor_t *reactor, int sockfd);
void event_init(event_t *ev, int fd, NCALLBACK callback, void *arg);
int event_add(int epfd, int events, event_t *ev);
int event_del(int epfd, event_t *ev);
int reactor_run(reactor_t *reactor);
int reactor_destory(reactor_t *reactor);



//函数实现
int reactor_init(reactor_t* re)
{
	if(NULL == re) return -1;
	memset(re,0,sizeof(reactor_t));

	re->epfd = epoll_create(1);
	if(re->epfd <= 0)
	{
		printf("[func:%s] create epfd err %s\n", __func__, strerror(errno));
		return -2;
	}

	//申请块内存
	evblk_t * evblk = (evblk_t *)malloc(MAX_EVENTBLOCKS*sizeof(evblk_t));
	if (NULL == evblk)
	{
		printf("[func:%s] reactor_alloc evblk_t failed\n",__func__);
		return -2;
	}
	memset(evblk,0,sizeof(evblk_t));

	//节点内存
	event_t * evs = (event_t *)malloc(sizeof(event_t));
	if (NULL == evs)
	{
		printf("[func:%s] reactor_alloc event_t failed\n",__func__);
		return -2;
	}
	memset(evs,0,sizeof(event_t));

	re->blkcnt = 1;
	re->evblk = evblk;

	evblk->events = evs;
	evblk->next = NULL;

	return 0;
}
int init_sock(uint8 port)
{

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	fcntl(fd, F_SETFL, O_NONBLOCK);

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(port);

	bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));

	if (listen(fd, 20) < 0)
	{
		printf("listen failed : %s\n", strerror(errno));
	}

	return fd;
}

void event_init(event_t *ev, int fd, NCALLBACK callback, void *arg)
{

	ev->fd = fd;
	ev->callback = callback;
	ev->events = 0;
	ev->arg = arg;
	ev->last_active = time(NULL);

	return ;
}
int event_add(int epfd, int events, event_t *ev)
{
	struct epoll_event ep_ev = {0, {0}};
	ep_ev.data.ptr = ev;
	ep_ev.events = ev->events = events;

	int op;
	if (ev->status == 1)
	{
		op = EPOLL_CTL_MOD;
	}
	else
	{
		op = EPOLL_CTL_ADD;
		ev->status = 1;
	}

	if (epoll_ctl(epfd, op, ev->fd, &ep_ev) < 0)
	{
		printf("[func:%s] event add failed [client fd=%d], events[%d]\n",__func__, ev->fd, events);
		return -1;
	}

	return 0;
}

int reactor_addlistener(reactor_t *re, int sockfd, NCALLBACK *acceptor)
{
	if (re == NULL) return -1;
	if (re->evblk == NULL) return -1;

	event_t *event = reactor_idx(re,sockfd);
	event_init(event, sockfd, acceptor, re);
	event_add(re->epfd, EPOLLIN, event);
}
event_t *reactor_idx(reactor_t *reactor, int sockfd)
{

	int blkidx = sockfd / MAX_EPOLL_EVENTS; //查找此客户端需要放到第几块

	while (blkidx >= reactor->blkcnt) //需要重新申请块
	{
		//ntyreactor_alloc(reactor);
	}

	int i = 0;
	evblk_t *blk = reactor->evblk;
	while(i++ < blkidx && blk != NULL)
	{
		blk = blk->next;
	}

	return &blk->events[sockfd % MAX_EPOLL_EVENTS]; //找到此客户端所在块的具体idx
}

int accept_cb(int fd, int events, void *arg)
{

	reactor_t *reactor = (reactor_t*)arg;
	if (reactor == NULL) return -1;

	struct sockaddr_in client_addr;
	socklen_t len = sizeof(client_addr);

	int clientfd;

	if ((clientfd = accept(fd, (struct sockaddr*)&client_addr, &len)) == -1)
	{
		if (errno != EAGAIN && errno != EINTR)
		{

		}
		printf("accept: %s\n", strerror(errno));
		return -1;
	}



	int flag = 0;
	if ((flag = fcntl(clientfd, F_SETFL, O_NONBLOCK)) < 0)
	{
		printf("[func:%s]: fcntl nonblocking failed, %d\n", __func__, MAX_EPOLL_EVENTS);
		return -1;
	}

	event_t *event = reactor_idx(reactor, clientfd);

	event_init(event, clientfd, recv_cb, reactor);
	event_add(reactor->epfd, EPOLLIN, event);


	printf("[func:%s]: new connect [%s:%d], pos[%d]\n",__func__,
		inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), clientfd);

	return 0;

}
int send_cb(int fd, int events, void *arg)
{
	reactor_t *reactor = (reactor_t*)arg;
	event_t *ev = reactor_idx(reactor, fd);

//	http_response(ev);
	//
	int len = send(fd, ev->buffer, ev->length, 0);
	if (len > 0)
	{
		printf("[func:%s]: send[fd=%d], [%d]%s\n",__func__, fd, len, ev->buffer);

		if (ev->ret_code == 200) {
			int filefd = open(ev->resource, O_RDONLY);
			struct stat stat_buf;
			fstat(filefd, &stat_buf);

			sendfile(fd, filefd, NULL, stat_buf.st_size);
			close(filefd);
		}


		event_del(reactor->epfd, ev);
		event_init(ev, fd, recv_cb, reactor);
		event_add(reactor->epfd, EPOLLIN, ev);

	} else {

		close(ev->fd);

		event_del(reactor->epfd, ev);
		printf("send[fd=%d] error %s\n", fd, strerror(errno));

	}

	return len;
}

int recv_cb(int fd, int events, void *arg)
{

	reactor_t *reactor = (reactor_t*)arg;
	event_t *ev = reactor_idx(reactor, fd);

	int len = recv(fd, ev->buffer, BUFFER_LENGTH, 0); //

	if (len > 0)
	{
		ev->length = len;
		ev->buffer[len] = '\0';

		printf("[func:%s]: [client fd:%d]:%s\n", __func__,fd, ev->buffer); //http

//		http_request(ev);

		//send();

		event_del(reactor->epfd, ev);
		event_init(ev, fd, send_cb, reactor);
		event_add(reactor->epfd, EPOLLOUT, ev);


	}
	else if (len == 0)
	{
		event_del(reactor->epfd, ev);
		close(ev->fd);
		//printf("[fd=%d] pos[%ld], closed\n", fd, ev-reactor->events);
		printf("[func:%s]: [client fd:%d]:%s,close\n", __func__,fd, ev->buffer); //http
	}
	else
	{
		event_del(reactor->epfd, ev);
		close(ev->fd);
		printf("[func:%s]: recv[fd=%d] error[%d]:%s\n", __func__, fd, errno, strerror(errno));

	}

	return len;
}

int event_del(int epfd, event_t *ev)
{
	struct epoll_event ep_ev = {0, {0}};

	if (ev->status != 1)
	{
		return -1;
	}

	ep_ev.data.ptr = ev;
	ev->status = 0;
	epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, &ep_ev);

	return 0;
}

int reactor_destory(reactor_t *reactor)
{

	close(reactor->epfd);
	//free(reactor->events);

	evblk_t *blk = reactor->evblk;
	evblk_t *blk_next = NULL;

	while (blk != NULL)
	{
		blk_next = blk->next;

		free(blk->events);
		free(blk);

		blk = blk_next;
	}

	return 0;
}

int reactor_run(reactor_t *reactor)
{
	if (reactor == NULL) return -1;
	if (reactor->epfd < 0) return -1;
	if (reactor->evblk == NULL) return -1;

	struct epoll_event events[MAX_EPOLL_EVENTS+1];

	int checkpos = 0, i;

	while (1)
	{
		int nready = epoll_wait(reactor->epfd, events, MAX_EPOLL_EVENTS, -1);
		if (nready < 0)
		{
			printf("epoll_wait error, exit\n");
			continue;
		}

		for (i = 0;i < nready;i ++)
		{
			event_t *ev = (event_t*)events[i].data.ptr;

			if ((events[i].events & EPOLLIN) && (ev->events & EPOLLIN))
			{
				ev->callback(ev->fd, events[i].events, ev->arg);
			}
			if ((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT))
			{
				ev->callback(ev->fd, events[i].events, ev->arg);
			}
		}
	}
}
int main(int argc, char *argv[])
{
	uint8 port = LISTEN_PORT;
	if(argc == 2)
	{
		port = atoi(argv[1]);
	}
	printf("[func:%s] listen port : %d\n",__func__,port);

	reactor_t *reactor = (reactor_t *)malloc(sizeof(reactor_t));
	reactor_init(reactor);

	//初始化socket并添加到监听队列
	int sockfds[PORT_COUNT] = {0};
	for(int i =0 ;i < PORT_COUNT; i++)
	{
		sockfds[i] = init_sock(port);
		reactor_addlistener(reactor, sockfds[i], accept_cb);
	}
	//
	reactor_run(reactor);

	//释放reactor资源
	reactor_destory(reactor);

	//关闭socket
	for(int i =0 ;i < PORT_COUNT; i++)
	{
		close(sockfds[i]);
	}
	//
	free(reactor);
}



