#include "tcp_sock.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// tcp server application, listens to port (specified by arg) and serves only one
// connection request
void *tcp_server(void *arg)
{
	u16 port = *(u16 *)arg;
	struct tcp_sock *tsk = alloc_tcp_sock();

	struct sock_addr addr;
	addr.ip = htonl(0);
	addr.port = port;
	if (tcp_sock_bind(tsk, &addr) < 0)
	{
		log(ERROR, "tcp_sock bind to port %hu failed", ntohs(port));
		exit(1);
	}

	if (tcp_sock_listen(tsk, 3) < 0)
	{
		log(ERROR, "tcp_sock listen failed");
		exit(1);
	}

	log(DEBUG, "listen to port %hu.", ntohs(port));

	struct tcp_sock *csk = tcp_sock_accept(tsk);

	log(DEBUG, "accept a connection.");

	char rbuf[1001];
	char wbuf[1024];
	int rlen = 0;
	while (1)
	{
		rlen = tcp_sock_read(csk, rbuf, 1000);
		if (rlen == 0)
		{
			log(DEBUG, "tcp_sock_read return 0, finish transmission.");
			break;
		}
		else if (rlen > 0)
		{
			rbuf[rlen] = '\0';
			sprintf(wbuf, "server echoes: %s", rbuf);
			if (tcp_sock_write(csk, wbuf, strlen(wbuf)) < 0)
			{
				log(DEBUG, "tcp_sock_write return negative value, something goes wrong.");
				exit(1);
			}
		}
		else
		{
			log(DEBUG, "tcp_sock_read return negative value, something goes wrong.");
			exit(1);
		}
	}

	log(DEBUG, "close this connection.");

	tcp_sock_close(csk);

	return NULL;
}

// tcp client application, connects to server (ip:port specified by arg), each
// time sends one bulk of data and receives one bulk of data
void *tcp_client(void *arg)
{
	struct sock_addr *skaddr = arg;

	struct tcp_sock *tsk = alloc_tcp_sock();

	if (tcp_sock_connect(tsk, skaddr) < 0)
	{
		log(ERROR, "tcp_sock connect to server (" IP_FMT ":%hu)failed.",
			NET_IP_FMT_STR(skaddr->ip), ntohs(skaddr->port));
		exit(1);
	}

	char *wbuf = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	int wlen = strlen(wbuf);
	char rbuf[1001];
	int rlen = 0;

	int n = 10;
	for (int i = 0; i < n; i++)
	{
		if (tcp_sock_write(tsk, wbuf + i, wlen - n) < 0)
			break;

		rlen = tcp_sock_read(tsk, rbuf, 1000);
		if (rlen == 0)
		{
			log(DEBUG, "tcp_sock_read return 0, finish transmission.");
			break;
		}
		else if (rlen > 0)
		{
			rbuf[rlen] = '\0';
			fprintf(stdout, "%s\n", rbuf);
		}
		else
		{
			log(DEBUG, "tcp_sock_read return negative value, something goes wrong.");
			exit(1);
		}
		sleep(1);
	}

	tcp_sock_close(tsk);

	return NULL;
}

void *tcp_client_file_ver(void *arg)
{
	struct sock_addr *skaddr = arg;

	struct tcp_sock *tsk = alloc_tcp_sock();

	if (tcp_sock_connect(tsk, skaddr) < 0)
	{
		log(ERROR, "tcp_sock connect to server (" IP_FMT ":%hu)failed.",
			NET_IP_FMT_STR(skaddr->ip), ntohs(skaddr->port));
		exit(1);
	}
	char *fname = "client-input.dat";
	FILE *fp = fopen(fname, "r");
	if (fp == NULL)
	{
		log(ERROR, "Open file %s failed", fname);
		return NULL;
	}
	char buf[1024];
	int tot_sz, rd_sz_acc = 0, rd_sz_cur;
	fseek(fp, 0, SEEK_END);
	tot_sz = ftell(fp);
	rewind(fp);
	while (rd_sz_acc < tot_sz)
	{
		rd_sz_cur = fread(buf, 1, 1024, fp);
		int wt_sz = tcp_sock_write(tsk, buf, rd_sz_cur);
		rd_sz_acc += wt_sz;
		if (wt_sz < rd_sz_cur)
		{
			fseek(fp, rd_sz_acc, SEEK_SET);
		}
		if (feof(fp))
		{
			break;
		}
		usleep(30000);
	}
	fclose(fp);
	log(DEBUG, "Client sending file ends.");
	log(DEBUG, "Close connection.");
	sleep(1);
	tcp_sock_close(tsk);
	return NULL;
}

void *tcp_server_file_ver(void *arg)
{
	u16 port = *(u16 *)arg;
	struct tcp_sock *tsk = alloc_tcp_sock();

	struct sock_addr addr;
	addr.ip = htonl(0);
	addr.port = port;
	if (tcp_sock_bind(tsk, &addr) < 0)
	{
		log(ERROR, "tcp_sock bind to port %hu failed", ntohs(port));
		exit(1);
	}

	if (tcp_sock_listen(tsk, 3) < 0)
	{
		log(ERROR, "tcp_sock listen failed");
		exit(1);
	}

	log(DEBUG, "listen to port %hu.", ntohs(port));

	struct tcp_sock *csk = tcp_sock_accept(tsk);

	log(DEBUG, "accept a connection.");

	char *fname = "server-output.dat";
	FILE *fp = fopen(fname, "w");
	char buf[1024];
	while (1)
	{
		int rlen = tcp_sock_read(csk, buf, 1024);
		if (rlen == 0)
		{
			log(DEBUG, "tcp_sock_read return 0, finish transmission.");
			break;
		}
		else if (rlen > 0)
		{
			fwrite(buf, 1, rlen, fp);
		}
		else
		{
			log(DEBUG, "tcp_sock_read return negative value, something goes wrong.");
			exit(1);
		}
		usleep(1000);
	}
	fclose(fp);
	log(DEBUG, "Server receiving file ends.");
	sleep(5);
	tcp_sock_close(csk);
	return NULL;
}