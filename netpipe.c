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

//socket服务器的设置
#define SERVER_ADDR "0.0.0.0"
#define SERVER_PORT 30000
#define SERVER_BACKLOG 1

//两程序之间的通讯管道
#define FIFO_PATH "/tmp/netpipe.fifo"
#define FIFO_MODE 0666

//记录两程序的pid
pid_t pid_in = 0, pid_out = 0;

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
	fprintf(stderr, "\npid:%d |->ERR: %s %s\n", getpid(), err_msg, strerror(errno));
	exit(1);
}



/*-----------------------------------------------------------------------------
 *  打印调试信息log_msg到stderr
 *-----------------------------------------------------------------------------*/
void logger(char log_msg[])
{
	//	time_t now;
	//	struct tm *timenow;
	//	time(&now);
	//	timenow = localtime(&now);
	//	fprintf(stderr, "%s|->LOG:%s ", asctime(timenow), log_msg );
	fprintf(stderr, "pid:%d |->LOG: %s\n", getpid(), log_msg );
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
		sprintf(logbuf, "child %d exit status %d", pid_child, WEXITSTATUS(stat_child));
		logger(logbuf);
	}
}


/*-----------------------------------------------------------------------------
 *  当收到要程序退出的信号时的处理器
 *-----------------------------------------------------------------------------*/
void handler_exit(int signal)
{
	close(server_fd);
	if(access(FIFO_PATH, F_OK) == 0)
	{
		if(unlink(FIFO_PATH) == -1)
		{
			error("unlink() fifo error.");
		}	
	}
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
 *  把server_fd，初始化为一个被动socket
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
	sa_in.sin_port = htons(SERVER_PORT);
	sa_in.sin_addr.s_addr = inet_network(SERVER_ADDR);
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
			sprintf(logbuf, "accept() ok, client:%s:%d.", inet_ntoa(sa_in.sin_addr), ntohs(sa_in.sin_port));
			logger(logbuf);
			//accept成功，跳出循环
			break;
		}
	}
	return client_fd;
}


/*-----------------------------------------------------------------------------
 *  从sockfd接受制定长度len的字符串到buf，可指定结束标志字符esc，返回接收的字符
 *  个数
 *-----------------------------------------------------------------------------*/
int server_recv(int sockfd, char buf[], char esc, int len)
{
	char *s = buf;
	int slen = len;
	int c = 0;

	//首先要清空缓冲区中内容
	memset(buf, 0, len);

	do{
		//s为接收字符的缓冲区，slen为期望获取到的字符个数，但是实际获取到的个数为c。
		c = recv(sockfd, s, slen, 0);
		//判断是否已经接收完毕，是则跳出循环。
		if( s[ c - 1 ] == esc)
		{
			s += c;
			slen -= c;
			break;
		}
		if(c > 0)
		{
			s += c;
			slen -= c;
		}
	}while( (c > 0) && (slen > 0) );
	//当c小于0时，代表发生了错误，或者是对方已关闭。

	if( c < 0 )
	{
		//发生了错误，需要检查errno
		return c;
	}else if( c == 0 ){
		//对方已关闭，返回空字符串
		buf[0] = '\0';
	}else{
		//把'\n'替换成'\0'，代表字符串结束。
		s[c - 1] = '\0';
	}
	return len - slen;
}


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
	int fifo_read = 0;

	if(argv[0] == NULL)
	{
		logger("prog in argv invalid.");
		return -1;
	}

	int i = 0;
	for(i = 0; argv[i] != NULL; i++)
	{
		logger(argv[i]);
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
		//打开管道，获得管道的描述符
		fifo_read = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
		//		fifo_read = open(FIFO_PATH, O_RDONLY);
		if(fifo_read == -1)
		{
			error("open() fifo for read error.");
		}
		logger("open() fifo for read ok.");


		//把stdin重定向到管道，那么从stdin读数据就相当与在管道中读数据了
		if(dup2( fifo_read, fileno(stdin)) == -1)
		{
			error("dup2() prog_in error.");
		}

		logger("fork for prog_in ok.");

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
	int fifo_write = 0;

	if(argv[0] == NULL)
	{
		logger("prog out argv invalid.");
		return -1;
	}

	int i = 0;
	for(i = 0; argv[i] != NULL; i++)
	{
		logger(argv[i]);
	}

	//判断之前的实例是否还在运行，是则先杀死
	if( (pid_out != 0) && (kill(pid_out, 0) == 0) )
	{
		kill(pid_out, SIGTERM);
		usleep(1000000);
	}

	pid_out = fork();
	if(pid_out == 0)
	{
		//打开管道，获得管道的描述符
		fifo_write = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
		//		fifo_write = open(FIFO_PATH, O_WRONLY);
		if(fifo_write == -1)
		{
			error("fopen() fifo for write error.");
		}
		logger("open() fifo for write ok.");

		//把stdin重定向到管道，那么向stdin写数据就相当与向管道中写数据了
		if(dup2(fifo_write, fileno(stdout)) == -1)
		{
			error("dup2() prog_out error.");
		}

		logger("fork for prog_out ok.");

		if(execvp(argv[0], argv) == -1)
		{
			error("fork for prog_out error.");
		}
	}

	return 0;
}


int main(int argc, char * args[])
{
	//打印版本信息
	logger(NETPIPE_BANNER);

	//设置信号处理器
	signal_catch(SIGCHLD, handler_sigchld);
	signal_catch(SIGINT, handler_exit);
	signal_catch(SIGTERM, handler_exit);

	//创建命名管道
	if(access(FIFO_PATH, F_OK) == 0)
	{
		if(unlink(FIFO_PATH) == -1)
		{
			error("unlink() fifo error.");
		}	
	}

	if(mkfifo(FIFO_PATH, FIFO_MODE) == -1)
	{
		error("mkfifo() error.");
	}

	//	char in[1024] = "nc -l 1234";
	//	args2argv(in, prog_in_argv, " ");
	//	char out[1024] = "nc -l 1235";
	//	args2argv(out, prog_out_argv, " ");
	//	fork_prog_in(prog_in_argv);
	//	fork_prog_out(prog_out_argv);
	//
	//	wait(NULL);


	//	while(1)
	//	{
	//		if((pid_in == 0) || (kill(pid_in, 0) != 0))
	//		{
	//			pid_in = fork();
	//			if(pid_in == 0)
	//			{
	//				fork_prog_in(prog_in_argv);
	//			}
	//		}
	//
	//		if((pid_out == 0) || (kill(pid_out, 0) != 0))
	//		{
	//			pid_out = fork();
	//			if(pid_out == 0)
	//			{
	//				fork_prog_out(prog_out_argv);
	//			}
	//		}
	//
	//		wait(NULL);
	//	}



	//初始化server_fd
	server_create(&server_fd, SERVER_ADDR, SERVER_PORT);

	//进入服务器循环
	while(1)
	{
		int client_fd = server_accept(server_fd);
		//发送欢迎信息
		send(client_fd, NETPIPE_BANNERNL, strlen(NETPIPE_BANNERNL), 0);
		int recv_byte = 0;

		enum PROG_NUM {
			PROG_IN,
			PROG_OUT,
			PROG_NONE
		} prog_num;

#define S_PROG_IN "prog_in"
#define S_PROG_OUT "prog_out"
#define S_ASK_PROG "which prog?\n" 
#define S_ASK_ARGS "what args?\n"

		while(1)
		{
			send(client_fd, S_ASK_PROG, strlen(S_ASK_PROG), 0);
			server_recv(client_fd, sockbuf, '\n', sizeof(sockbuf));

			//把要启动的程序放到prog_num
			if(strncmp(sockbuf, S_PROG_IN, strlen(S_PROG_IN)) == 0)
			{
				prog_num = PROG_IN;
			}else if(strncmp(sockbuf, S_PROG_OUT, strlen(S_PROG_OUT)) == 0)
			{
				prog_num = PROG_OUT;
			}else{
				continue;
			}

			//把程序的参数存进sockbuf
			send(client_fd, S_ASK_ARGS, strlen(S_ASK_ARGS), 0);
			recv_byte = server_recv(client_fd, sockbuf, '\n', sizeof(sockbuf));

			if(recv_byte <= 1)
			{
				continue;
			}

			if(prog_num == PROG_IN)
			{
				puts(S_PROG_IN);
				puts(sockbuf);
				memset(prog_in_args, 0, sizeof(prog_in_args));
				strncpy(prog_in_args, sockbuf, recv_byte);
				args2argv(prog_in_args, prog_in_argv, " ");
				fork_prog_in(prog_in_argv);
			}else if(prog_num == PROG_OUT)
			{
				puts(S_PROG_OUT);
				puts(sockbuf);
				memset(prog_out_args, 0, sizeof(prog_out_args));
				strncpy(prog_out_args, sockbuf, recv_byte);
				args2argv(prog_out_args, prog_out_argv, " ");
				fork_prog_out(prog_out_argv);
			}

			prog_num = 0;

		}


		close(client_fd);

	}


	return EXIT_SUCCESS;
}
