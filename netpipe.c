#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include "netpipe.h"

//版本信息
#define NETPIPE_BANNER "netpipe v1.0"
#define NETPIPE_BANNERNL "netpipe v1.0\n"

//socket服务器的设置
#define SERVER_ADDR "000.000.000.000"
#define SERVER_PORT 30000
#define SERVER_BACKLOG 1

//协议内容
#define S_PROG_IN "in"
#define S_PROG_OUT "out"
#define S_ASK_PROG "which prog?\n"
#define S_ASK_ARGS "what args?\n"

//兄弟进程之间的通讯管道的fd，pipe_r和pipe_w是为了使用更加直观
int pipe_fd[2];
int pipe_r = 0, pipe_w;


//记录pid
pid_t pid_in = 0, pid_out = 0, pid_main = 0;

//socket服务端的描述符
int server_fd = 0;

//用于存放log信息的缓冲区
char logbuf[1024] = "";
char sockbuf[1024] = "";

//两程序的参数以及参数向量的缓冲区
char prog_in_args[1024] = "", *prog_in_argv[64];
char prog_out_args[1024] = "", *prog_out_argv[64];



/*-----------------------------------------------------------------------------
 *  处理致命错误的函数，输出错误信息err_msg到stderr，然后退出程序，错误状态为1
 *-----------------------------------------------------------------------------*/
void error(char err_msg[])
{
	fprintf(stderr, "--------------------------------------------------\n");
	fprintf(stderr, "%4d->ERR: %s %s\n", getpid(), err_msg, strerror(errno));
	fprintf(stderr, "--------------------------------------------------\n");
	exit(1);
}



/*-----------------------------------------------------------------------------
 *  打印调试信息log_msg到stderr
 *-----------------------------------------------------------------------------*/
void logger(char log_msg[])
{
	if(getpid() == pid_main)
	{
		fprintf(stderr, "%4d->LOG: %s\n", getpid(), log_msg );
	}
	else
	{
		fprintf(stderr, " %4d|>LOG: %s\n", getpid(), log_msg );
	}
}


/*-----------------------------------------------------------------------------
 *  打印帮助信息然后退出
 *-----------------------------------------------------------------------------*/
void usage(void)
{
	fprintf(stderr,
			"netpipe  [option]...\n"
			"	-a address\n"
			"	-p port\n"
			"	-h usage\n"
			"\n"
			"example: netpipe -a 127.0.0.1 -p 1234\n"
		   );
	fprintf(stderr,
			"default addr:%s port:%d\n",
			SERVER_ADDR, SERVER_PORT);
	exit(EXIT_FAILURE);
}

/*-----------------------------------------------------------------------------
 *  设置系统信号处理器,把handler作为signal信号的处理器。
 *-----------------------------------------------------------------------------*/
int signal_catch(int signal, void (*handler) (int))
{
	struct sigaction action;
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	return sigaction(signal, &action, NULL);

}



/*-----------------------------------------------------------------------------
 *  处理SIGCHLD信号的函数，如果不处理那么子进程就会变成僵尸进程
 *-----------------------------------------------------------------------------*/
void handler_sigchld(int signal)
{
	pid_t pid_child = 0;
	int stat_child = 0;
	while( ( pid_child = waitpid( -1, &stat_child, WNOHANG )) > 0)
	{
		if(pid_child == pid_in)
		{

			sprintf(logbuf, "%s pid:%d exit status %d", "prog_in", pid_child, WEXITSTATUS(stat_child));
		}else if(pid_child == pid_out)
		{
			sprintf(logbuf, "%s pid:%d exit status %d", "prog_out", pid_child, WEXITSTATUS(stat_child));
		}
		logger(logbuf);
	}
}


/*-----------------------------------------------------------------------------
 *  当收到要程序退出的信号时的处理器
 *-----------------------------------------------------------------------------*/
void handler_exit(int signal)
{
	close(server_fd);
	logger("exit.");
	exit(0);
}


/*-----------------------------------------------------------------------------
 *  把字符串args_in[]中的每个参数以字符串split分割，每个参数的位置依次放在
 *  指针数组argv_out[]中，以便作为execvp()的参数，返回参数的个数
 *-----------------------------------------------------------------------------*/
int args2argv(char args_in[], char *argv_out[], char split[])
{
	char *token = NULL;
	int i = 0;

	//开始分割参数，第一次调用strtok需指定所要分割的字符串
	token = strtok(args_in, split);
	for(i = 0; token != NULL; i++)
	{
		argv_out[i] = token;
		//之后调用strtok就不需要再指定所要分割的字符串了
		token = strtok(NULL, split);
	}

	//在argv_out参数列表的最后插入NULL，标志着参数的结束
	argv_out[i] = NULL;

	return i;
}


/*-----------------------------------------------------------------------------
 *  把server_fd，初始化为一个被动socket，addr和port都为主机字节序列
 *-----------------------------------------------------------------------------*/
void server_create(int *socket_fd, char addr[], uint16_t port)
{
	//初始化socket
	*socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if(*socket_fd == -1)
	{
		error("socket() fail.");
	}else{
		logger("socket() ok.");
	}

	//设置相应的socket参数，避免频繁重启时出现Address already in use错误
	int reuse = 1;
	if(setsockopt(*socket_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &reuse, sizeof(int)) == -1)
	{
		error("setsockopt() faild.");
	}else{
		logger("setsockopt() ok.");
	}

	//为服务器绑定一个地址
	struct sockaddr_in sa_in = {};
	sa_in.sin_family = AF_INET;
	sa_in.sin_port = htons(port);
	if(inet_aton(addr, &sa_in.sin_addr) == 0)
	{
		error("inet_aton() error, the address is invalid.");
	}

	if(bind(*socket_fd, (struct sockaddr *) &sa_in, sizeof(sa_in)) == -1)
	{
		error("bind() fail.");
	}else{
		sprintf(logbuf, "bind() ok, server:%s:%d.", inet_ntoa(sa_in.sin_addr), ntohs(sa_in.sin_port));
		logger(logbuf);
	}

	//设置服务器socket为监听
	if(listen(*socket_fd, SERVER_BACKLOG) == -1)
	{
		error("listen() fail.");
	}else{
		sprintf(logbuf, "listen() ok, backlog:%d.", SERVER_BACKLOG);
		logger(logbuf);
	}

}


/*-----------------------------------------------------------------------------
 *  给定一个被动模式的socket描述符，返回一个已连接的socket的描述符
 *-----------------------------------------------------------------------------*/
int server_accept(int socket_fd)
{
	int client_fd = 0;
	struct sockaddr_in sa_in = {};
	socklen_t sa_in_l = sizeof(sa_in);
	//由于accept()阻塞的过程中可能会被其他信号打断而错误，需要一个循环来处理
	while(1)
	{
		client_fd = accept(socket_fd, (struct sockaddr *) &sa_in, &sa_in_l);
		if(client_fd == -1)
		{
			//如果accept不是因为被其他信号所打断而失败的话，就报错退出程序
			if(errno != EINTR)
			{
				error("accpet() fail.");
			}
		}else{
			logger("----------------------------------------");
			sprintf(logbuf, "accept() ok, client:%s:%d.", inet_ntoa(sa_in.sin_addr), ntohs(sa_in.sin_port));
			logger(logbuf);
			//accept成功，跳出循环
			break;
		}
	}
	return client_fd;
}


/*-----------------------------------------------------------------------------
 *  从sockfd接受特定长度的len的字符串到buf，可指定结束标志字符esc，返回接收的字符
 *  个数，函数返回有两种情况，情况1是接受到了esc字符，情况2是达到了期望接受的字符
 *-----------------------------------------------------------------------------*/
int socket_recv(int sockfd, char buf[], char esc, int len)
{
	//指向下次接收的起始位置的指针
	char *s = buf;
	//slen，剩余的想要接收的字符串的长度
	int slen = len;
	//某次接受到的字符长度
	int c = 0;

	//首先要清空缓冲区中内容
	memset(buf, 0, len);

	do{
		//s为接收字符的缓冲区，slen为期望获取到的字符个数，实际获取到的个数为c。
		c = recv(sockfd, s, slen, 0);

		//c > 0则判断是否已经接收到结束字符
		if(c > 0)
		{
			s += c;
			slen -= c;
			//判断是否已经接收完毕，是则跳出循环。
			if( buf[len - slen - 1]  == esc)
			{
				//把结束字符esc替换成'\0'，代表字符串结束。
				buf[len - slen - 1] = '\0';
				break;
			}
		}
		else if(c == 0)
		{
			//若对方已经关闭则返回0
			buf[0] = '\0';
			logger("peer orderly shutdown.");
			return 0;
		}
		else if(c == -1)
		{
			//若出错则直接返回错误码-1
			buf[0] = '\0';
			sprintf(logbuf, "recv error %s.", strerror(errno));
			logger(logbuf);
			return -1;
		}

	}while( slen > 0 );

	sprintf(logbuf, "recv: \"%s\" (%dbyte).", buf, len - slen);
	logger(logbuf);

	return len - slen;
}


/*-----------------------------------------------------------------------------
 *  向指定的sockfd发送，buf中len长度的信息，如果出错则break
 *-----------------------------------------------------------------------------*/

#define server_send( socket_fd, buf, len) \
	if(send(socket_fd, buf, len, 0) == -1)\
{\
	sprintf(logbuf, "send() error %s", strerror(errno));\
	logger(logbuf);\
	break;\
}\



/*-----------------------------------------------------------------------------
 *  从sockfd中接收len长度的数据到buf，如果遇到esc字符就会返回，如果出错则break
 *-----------------------------------------------------------------------------*/
#define server_recv(sockfd, buf, esc, len) \
	if(socket_recv(sockfd, buf, esc, len) <= 0 )\
{\
	break;\
}\

/*-----------------------------------------------------------------------------
 *  向相应的socket发送调试信息，同时也会在本地显示
 *-----------------------------------------------------------------------------*/

int server_logger(int socket_fd, char msg[])
{
	send(socket_fd, msg, strlen(msg), 0);
	logger(msg);
	return 0;
}

/*-----------------------------------------------------------------------------
 *  从管道读取数据的程序fork后的操作
 *-----------------------------------------------------------------------------*/
int fork_prog_in(char *argv[])
{
	if(argv[0] == NULL)
	{
		logger("prog in argv invalid.");
		return -1;
	}

	//判断之前的实例是否还在运行，是则先杀死
	if( (pid_in != 0) && (kill(pid_in, 0) == 0) )
	{
		kill(pid_in, SIGTERM);
		usleep(1000000);
	}


	pid_in = fork();
	if(pid_in == 0)
	{
		//把stdin重定向到管道，那么从stdin读数据就相当与在管道中读数据了
		if(dup2( pipe_r, fileno(stdin)) == -1)
		{
			error("dup2() prog_in error.");
		}else
		{
			logger("dup2() prog_in ok.");
		}

		logger("fork for prog_in.");
		if(execvp(argv[0], argv) == -1)
		{
			error("fork for prog_in error.");
		}
	}

	return 0;
}


/*-----------------------------------------------------------------------------
 *  向管道写入数据的程序fork后的操作
 *-----------------------------------------------------------------------------*/
int fork_prog_out(char *argv[])
{
	if(argv[0] == NULL)
	{
		logger("prog out argv invalid.");
		return -1;
	}

	//判断之前的实例是否还在运行，是则先杀死
	if( (pid_out != 0) && (kill(pid_out, 0) == 0) )
	{
		kill(pid_out, SIGTERM);
		usleep(100000);
	}

	pid_out = fork();
	if(pid_out == 0)
	{
		//把stdin重定向到管道，那么向stdin写数据就相当与向管道中写数据了
		if(dup2(pipe_w, fileno(stdout)) == -1)
		{
			error("dup2() prog_out error.");
		}else
		{
			logger("dup2() prog_out ok.");
		}

		logger("fork for prog_out.");
		if(execvp(argv[0], argv) == -1)
		{
			error("fork for prog_out error.");
		}
	}

	return 0;
}


int main(int argc, char * argv[])
{
	//记录要改变启动的程序
	enum PROG_NUM {
		PROG_IN,
		PROG_OUT,
		PROG_NONE
	} prog_num;

	int opt = 0;
	//记录要监听的ip和端口
	char addr[] = SERVER_ADDR;
	uint16_t port = SERVER_PORT;
	pid_main = getpid();


	//打印版本信息
	logger(NETPIPE_BANNER);

	while( (opt = getopt(argc, argv, "a:p:h")) != -1)
	{
		switch (opt)
		{
			case 'a':
				strncpy(addr, optarg, sizeof(addr) - 1);
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'h':
				usage();
				break;
			case '?':
				break;
			default:
				break;
		}
	}


	//设置信号处理器
	signal_catch(SIGCHLD, handler_sigchld);
	signal_catch(SIGINT, handler_exit);
	signal_catch(SIGTERM, handler_exit);

	//创建兄弟进程之间的通讯管道
	if(pipe(pipe_fd) == -1)
	{
		error("pipe() error.");
	}else{
		logger("pipe() ok.");
		//pipe_r,pipe_w是为了使用更加的直观
		pipe_r = pipe_fd[0];
		pipe_w = pipe_fd[1];
	}

	//初始化server_fd
	server_create(&server_fd, addr, port);


	//进入服务器循环
	while(1)
	{
		int client_fd = server_accept(server_fd);
		//发送欢迎信息
		send(client_fd, NETPIPE_BANNERNL, strlen(NETPIPE_BANNERNL), 0);

		while(1)
		{
			prog_num = PROG_NONE;
			//询问需要启动的程序
			server_send(client_fd, S_ASK_PROG, strlen(S_ASK_PROG));
			server_recv(client_fd, sockbuf, '\n', sizeof(sockbuf));

			//把要启动的程序记录到prog_num
			if(strncmp(sockbuf, S_PROG_IN, strlen(S_PROG_IN)) == 0)
			{
				prog_num = PROG_IN;
			}else if(strncmp(sockbuf, S_PROG_OUT, strlen(S_PROG_OUT)) == 0)
			{
				prog_num = PROG_OUT;
			}else{
				continue;
			}

			//询问程序的参数
			server_send(client_fd, S_ASK_ARGS, strlen(S_ASK_ARGS));
			server_recv(client_fd, sockbuf, '\n', sizeof(sockbuf));

			if(strlen(sockbuf) <= 1)
			{
				continue;
			}

			logger("client close.");
			//在fork之前要先关闭fd，否则在fork之后就无法关闭了，这样系统就会为fork后的进程保持
			//这个socket连接，导致断开以后无法重连
			close(client_fd);

			if(prog_num == PROG_IN)
			{
				memset(prog_in_args, 0, sizeof(prog_in_args));
				strncpy(prog_in_args, sockbuf, sizeof(prog_in_args));
				args2argv(prog_in_args, prog_in_argv, " ");
				fork_prog_in(prog_in_argv);
			}else if(prog_num == PROG_OUT)
			{
				memset(prog_out_args, 0, sizeof(prog_out_args));
				strncpy(prog_out_args, sockbuf, sizeof(prog_out_args));
				args2argv(prog_out_args, prog_out_argv, " ");
				fork_prog_out(prog_out_argv);
			}
			break;

		}

	}


	return EXIT_SUCCESS;
}
