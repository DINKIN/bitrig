/* $OpenBSD: ftp-proxy.c,v 1.1 2001/08/19 04:11:12 beck Exp $ */
/*
 * Copyright (c) 1996-2001
 *	Obtuse Systems Corporation.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Obtuse Systems nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY OBTUSE SYSTEMS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL OBTUSE SYSTEMS CORPORATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF 
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *   
 */

/*
 * ftp proxy, Originally based on juniper_ftp_proxy from the Obtuse
 * Systems juniper firewall, written by Dan Boulet <danny@obtuse.com>
 * and Bob Beck <beck@obtuse.com>
 *
 * This version basically passes everything through unchanged except
 * for the PORT and the * "227 Entering Passive Mode" reply.
 *
 * A PORT command is handled by noting the IP address and port number
 * specified and then configuring a listen port on some very high port
 * number and telling the server about it using a PORT message.
 * We then watch for an in-bound connection on the port from the server
 * and connect to the client's port when it happens.
 *
 * A "227 Entering Passive Mode" reply is handled by noting the IP address
 * and port number specified and then configuring a listen port on some
 * very high port number and telling the client about it using a
 * "227 Entering Passive Mode" reply.
 * We then watch for an in-bound connection on the port from the client
 * and connect to the server's port when it happens.
 *
 * supports tcp wrapper lookups/access control with the -w flag using
 * the real destination address - the tcp wrapper stuff is done after
 * the real destination address is retreived from pf
 * 
 */

/* 
 * TODO:
 * Plenty, this is very basic, with the idea to get it in clean first.
 * 
 * - Ipv6 and EPASV support 
 * - Content filter support
 * - filename filter support
 * - per-user rules perhaps.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>

#include <net/if.h>
#include <netinet/in.h>

#include <arpa/inet.h>

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <syslog.h>
#include <tcpd.h>
#include <unistd.h>

#include "util.h"

int allow_severity = LOG_INFO;
int deny_severity = LOG_NOTICE;

int min_port = IPPORT_HIFIRSTAUTO;
int max_port = IPPORT_HILASTAUTO;

#define STARTBUFSIZE  1024	/* Must be at least 3 */

/*
 * Variables used to support PORT mode connections.
 *
 * This gets a bit complicated.
 *
 * If PORT mode is on then client_listen_sa describes the socket that
 * the real client is listening on and server_listen_sa describes the
 * socket that we are listening on (waiting for the real server to connect
 * with us).
 *
 * If PASV mode is on then client_listen_sa describes the socket that
 * we are listening on (waiting for the real client to connect to us on)
 * and server_listen_sa describes the socket that the real server is
 * listening on.
 *
 * If the socket we are listening on gets a connection then we connect
 * to the other side's socket.  Similarily, if a connected socket is
 * shutdown then we shutdown the other side's socket.
 */

double xfer_start_time;

struct sockaddr_in real_server_sa;
struct sockaddr_in client_listen_sa;
struct sockaddr_in server_listen_sa;

int client_listen_socket = -1;	/* Only used in PASV mode */
int client_data_socket = -1;	/* Connected socket to real client */
int server_listen_socket = -1;	/* Only used in PORT mode */
int server_data_socket = -1;	/* Connected socket to real server */
int client_data_bytes, server_data_bytes;

int AnonFtpOnly;
int Verbose;
int NatMode;


char ClientName[NI_MAXHOST];
char RealServerName[NI_MAXHOST];
char OurName[NI_MAXHOST];

char *argstr = "m:M:D:t:AnVwr";

extern int Debug_Level;
extern int Use_Rdns;
extern char *__progname;

typedef enum {
	UNKNOWN_MODE,
	PORT_MODE,
	PASV_MODE,
	EPRT_MODE,
	EPSV_MODE
} connection_mode_t;

connection_mode_t connection_mode;

extern void debuglog(int debug_level, const char *fmt, ...);

static void
usage()
{
	syslog(LOG_NOTICE,
	    "usage: %s [-ArVw] [-t timeout] [-D debuglevel] %s",
	    __progname, "[-m min_port] [-M max_port ]\n");
	exit(EX_USAGE);
}

/* 
 * Check a connection against the tcpwrapper, log if we're going to
 * reject it, returns: 0 -> reject, 1 -> accept. We add in hostnames
 * if we are set to do reverse DNS, otherwise no.
 */

static int
check_host(struct sockaddr_in *client_sin, struct sockaddr_in *server_sin)
{

	char cname[NI_MAXHOST];
	char sname[NI_MAXHOST];
	struct request_info request;
	int i;

	request_init(&request, RQ_DAEMON, __progname, RQ_CLIENT_SIN, 
	    client_sin, RQ_SERVER_SIN, server_sin, RQ_CLIENT_ADDR,
	    inet_ntoa(client_sin->sin_addr), 0);

	if (Use_Rdns) {

		/*
		 * We already looked these up, but we have to do it again
		 * for tcp wrapper, to ensure that we get the DNS name, since
		 * the tcp wrapper cares about these things, and we don't
		 * want to pass in a printed address as a name.
		 */
		
		if (Use_Rdns)  {
			i = getnameinfo(
			    (struct sockaddr *) &client_sin->sin_addr,
			    sizeof(&client_sin->sin_addr), cname, 
			    sizeof(cname), NULL, 0, NI_NAMEREQD);
			if (i == -1)
				strlcpy(cname, STRING_UNKNOWN, sizeof(cname));

			i = getnameinfo(
			    (struct sockaddr *)&server_sin->sin_addr, 
			    sizeof(&server_sin->sin_addr), sname, 
			    sizeof(sname), NULL, 0, NI_NAMEREQD);
			if (i == -1)
				strlcpy(sname, STRING_UNKNOWN, sizeof(sname));
		} else {
			/*
			 * ensure the TCP wrapper doesn't start doing
			 * reverse DNS lookups if we aren't supposed to.
			 */
			strlcpy(cname, STRING_UNKNOWN, sizeof(cname));
			strlcpy(sname, STRING_UNKNOWN, sizeof(sname));
		}
	}
	
	request_set(&request, RQ_SERVER_ADDR, inet_ntoa(server_sin->sin_addr),
	    0);
	request_set(&request, RQ_CLIENT_NAME, cname, RQ_SERVER_NAME, sname, 0);

	if (!hosts_access(&request)) {
		syslog(LOG_NOTICE, "tcpwrappers rejected: %s -> %s",
		    ClientName, RealServerName);
		return(0);
	}
	return(1);
}

double
wallclock_time()
{
	struct timeval tv;
	
	gettimeofday(&tv,NULL);
	return(tv.tv_sec + tv.tv_usec / 1e6);
}

/*
 * Show the stats for this data transfer
 */

void
show_xfer_stats()
{
	char tbuf[1000];
	double delta;
	size_t len;
	int i;
	
	if (!Verbose)
		return;

	delta = wallclock_time() - xfer_start_time;
	
	if (delta < 0.001) 
		delta = 0.001;

	if (client_data_bytes == 0 && server_data_bytes == 0) {
		syslog(LOG_INFO,
		  "data transfer completed (no bytes transferred)");
		return;
	}
	
	len = sizeof(tbuf);

	if (delta >= 60) {
		int idelta;
		
		idelta = delta + 0.5;
		if (idelta >= 60*60) {
			i = snprintf(tbuf, len,
			    "data transfer completed (%dh %dm %ds",
			    idelta / (60*60), (idelta % (60*60)) / 60, 
			    idelta % 60);
			if (i >= len)
				goto logit;
			len -= i;
				
		}
		else {
			i = snprintf(tbuf, len, 
			    "data transfer completed (%dm %ds", idelta / 60,
			    idelta % 60);
			if (i >= len)
				goto logit;
			len -= i;
		}
	} else {
		i = snprintf(tbuf, len, "data transfer completed (%.1fs",
		    delta);
		if (i >= len)
			goto logit;
		len -= i;
	}

	if (client_data_bytes > 0) {
		i = snprintf(&tbuf[strlen(tbuf)], len, 
		    ", %d (%.1fKB/s) to server", client_data_bytes,
		    (client_data_bytes / delta) / (double)1024);
		if (i >= len)
			goto logit;
		len -= i;
	}
	if (server_data_bytes > 0) {
		i = snprintf(&tbuf[strlen(tbuf)], len,
		    ", %d (%.1fKB/s) to client", server_data_bytes,
		    (server_data_bytes / delta) / (double)1024);
		if (i >= len)
			goto logit;
		len -= i;
	}
	strlcat(tbuf,")", len);
 logit:	
	syslog(LOG_INFO,"%s",tbuf);
}

/*
 * Are we in PORT mode, PASV mode or unknown mode?
 */

void 
log_control_command (char * cmd, int client)
{
	/* log an ftp control command or reply */
	char * logstring;
	int level = LOG_DEBUG;

	if (!Verbose) 
		return;

	/* don't log passwords */
	if (strncasecmp(cmd,"pass ",5) == 0)
		logstring = "PASS XXXX";
	else
		logstring = cmd;
	if (client) {
		/* log interesting stuff at LOG_INFO, rest at LOG_DEBUG */
		if ((strncasecmp(cmd,"user ",5) == 0) || 
		    (strncasecmp(cmd,"retr ",5) == 0) ||
		    (strncasecmp(cmd,"cwd ",4) == 0) ||
		    (strncasecmp(cmd,"stor ",5) == 0))
			level = LOG_INFO;
	}
	syslog(level, "%s %s", (client?"from client:":"server reply:"),
	    logstring);
}


/*
 * set ourselves up for a new data connection. Direction is toward client if
 * "server" is 0, towards server otherwise.
 */

int
new_dataconn(int server)
{
	
	/*
	 * Close existing data conn.
	 */
	
	if (client_listen_socket != -1) {
		close(client_listen_socket);
		client_listen_socket = -1;
	}
	if (client_data_socket != -1) {
		shutdown(client_data_socket,2);
		close(client_data_socket);
		client_data_socket = -1;
	}
	if (server_listen_socket != -1) {
		close(server_listen_socket);
		server_listen_socket = -1;
	}
	if (server_data_socket != -1) {
		shutdown(server_data_socket,2);
		close(server_data_socket);
		server_data_socket = -1;
	}
	
	if (server) {
		bzero (&server_listen_sa, sizeof(server_listen_sa));
		
		server_listen_socket = get_backchannel_socket(
			SOCK_STREAM,
			min_port,
			max_port,
			-1,
			1,
		&server_listen_sa);
		
		if (server_listen_socket == -1) {
			syslog(LOG_INFO,"bind of server socket failed (%m)");
			exit(EX_OSERR);
		}
		if (listen(server_listen_socket, 5) != 0) {
			syslog(LOG_INFO,"listen on server socket failed (%m)");
			exit(EX_OSERR);
		}
		
	} else {
		bzero(&client_listen_sa, sizeof(client_listen_sa)); 
		
		client_listen_socket = get_backchannel_socket(SOCK_STREAM,
		    min_port, max_port, -1, 1, &client_listen_sa);

		if (client_listen_socket == -1) {
			syslog(LOG_NOTICE,
			    "can't get client listen socket (%m)");
			exit(EX_OSERR);
		}
		if (listen(client_listen_socket, 5) != 0) {
			syslog(LOG_NOTICE,
			    "can't listen on client socket (%m)");
			exit(EX_OSERR);
		}
	}
	return(0);
}

void
do_client_cmd(struct csiob *client, struct csiob *server)
{
	int i,j,rv;
	char tbuf[100];
	char *sendbuf = NULL; 

	log_control_command((char *)client->line_buffer, 1);
	
	/* client->line_buffer is an ftp control command.
	 * There is no reason for these to be very long.
	 * In the interest of limiting buffer overrun attempts,
	 * we catch them here. 
	 */
	
	if (strlen((char *)client->line_buffer) > 512) {
		syslog(LOG_NOTICE, "excessively long control command");
		exit(EX_DATAERR);
	}

	/*
	 * Check the client user provided if needed
	 */

	if (AnonFtpOnly && strncasecmp((char *)client->line_buffer,"user ",
	    strlen("user ")) == 0) {
		char * cp;
		cp = client->line_buffer + strlen("user ");
		if ((strcasecmp(cp,"ftp\r\n") != 0) 
		    && (strcasecmp(cp, "anonymous\r\n") != 0)) {
			/*
			 * this isn't anonymous - give the client an 
			 * error before they send a password
			 */
			snprintf(tbuf, sizeof(tbuf), 
			    "500 Only anonymous ftp is allowed\r\n");
			j = 0;
			i = strlen(tbuf);
			do {
				rv = send(client->fd, tbuf + j, i - j, 0);
				if (rv == -1 && errno != EAGAIN && 
				    errno != EINTR)
					break;
				else if (rv != -1)
					j += rv;
			} while (j >= 0 && j < i);
			sendbuf = NULL;
		} else 
			sendbuf = client->line_buffer;
	}

		
	/*
	 * Watch out for EPRT commands.
	 */
	
	else if ((strncasecmp((char *)client->line_buffer,"eprt ", 
 	    strlen("eprt ")) == 0)) { 

		char *line = NULL; 
		char *q, *p;
		char *result[3];
		char delim;
		struct addrinfo hints;
		struct addrinfo *res = NULL;
		unsigned long proto;
		
		j = 0;
		line = strdup(client->line_buffer+strlen("eprt "));
		if (line == NULL) {
			syslog(LOG_ERR, "insufficient memory");
			exit(EX_UNAVAILABLE);
		}
		p = line;
		delim = p[0];
		p++;

		memset(result,0, sizeof(result));
		for (i = 0; i < 3; i++) {
			q = strchr(p, delim);
			if (!q || *q != delim)
				goto parsefail;
			*q++ = '\0';
			result[i] = p;
			p = q;
		}

		proto = strtoul(result[0], &p, 10);
		if (!*result[0] || *p)
			goto protounsupp;

		memset(&hints, 0, sizeof(hints));
		if (proto != 1) /* 1 == AF_INET - all we support for now */
			goto protounsupp;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICHOST;	/*no DNS*/
		if (getaddrinfo(result[1], result[2], &hints, &res))
			goto parsefail;
		if (res->ai_next)
			goto parsefail;
		if (sizeof(client_listen_sa) < res->ai_addrlen)
			goto parsefail;
		memcpy(&client_listen_sa, res->ai_addr, res->ai_addrlen);
		
		debuglog(1,"client wants us to use %s:%u\n",
		    inet_ntoa(client_listen_sa.sin_addr), 
		    htons(client_listen_sa.sin_port));

		/*
		 * Configure our own listen socket and tell the server about it
		 */

		new_dataconn(1);
		
		connection_mode = EPRT_MODE;

		debuglog(1,"we want server to use %s:%u\n",
		    inet_ntoa(server->sa.sin_addr),
		    ntohs(server_listen_sa.sin_port));	 

		snprintf(tbuf, sizeof(tbuf), "EPRT |%d|%s|%u|\r\n", 1,
		    inet_ntoa(server->sa.sin_addr), 
		    ntohs(server_listen_sa.sin_port));		
		debuglog(1,"to server(modified):  %s",tbuf);
		sendbuf = tbuf;
		goto out;
parsefail:
		snprintf(tbuf, sizeof(tbuf), 
		    "500 Invalid argument, rejected\r\n");
		sendbuf = NULL;
		goto out;
protounsupp: 
		/* we only support AF_INET for now */
		if (proto == 2)
			snprintf(tbuf, sizeof(tbuf), 
			    "522 Protocol not supported, use (1)\r\n");
		else
			snprintf(tbuf, sizeof(tbuf), 
			    "501 Protocol not supported\r\n");
		sendbuf = NULL;
out:
		if (line)
			free(line);
		if (res)
			freeaddrinfo(res);
		if (sendbuf == NULL) {
			debuglog(1, "to client(modified):  %s\n",tbuf);
			i = strlen(tbuf);
			do {
				rv = send(client->fd, tbuf + j, i - j, 0);
				if (rv == -1 && errno != EAGAIN && 
				    errno != EINTR)
					break;
				else if (rv != -1)
					j += rv;
			} while (j >= 0 && j < i);
		}
	} else if (!NatMode && (strncasecmp((char *)client->line_buffer,"epsv",
	    strlen("epsv")) == 0)) { 

		/*
		 * If we aren't in NAT mode, deal with EPSV.
		 * EPSV is a problem - Unliks PASV, the reply from the 
		 * server contains *only* a port, we can't modify the reply
		 * to the client and get the client to connect to us without
		 * resorting to using a dynamic rdr rule we have to add in
		 * for the reply to this connection, and take away afterwards.
		 * so this will wait until we have the right solution for rule
		 * addtions/deletions in pf. 
		 *
		 * in the meantime we just tell the client we don't do it, 
		 * and most clients should fall back to using PASV.
		 */
		
		snprintf(tbuf, sizeof(tbuf), 
		    "500 EPSV command not understood\r\n");
		debuglog(1, "to client(modified):  %s\n",tbuf);
		j = 0;
		i = strlen(tbuf);
		do {
			rv = send(client->fd, tbuf + j, i - j, 0);
			if (rv == -1 && errno != EAGAIN && errno != EINTR)
				break;
			else if (rv != -1)
				j += rv;
		} while (j >= 0 && j < i);
		sendbuf = NULL;
	} else if (strncasecmp((char *)client->line_buffer,"port ", 
	    strlen("port ")) == 0) { 
		unsigned int values[6];
		int byte_number;
		u_char *tailptr, ch;

		debuglog(1,"Got a PORT command\n");
		
		tailptr = &client->line_buffer[strlen("port ")];
		byte_number = 0;
		values[0] = 0;
		while ((ch = *tailptr) == ',' || isdigit(ch)) {
			if (isdigit(ch)) {
				values[byte_number] = values[byte_number]
				  * 10 + ch - '0';
				if (values[byte_number] > 255) {
					syslog(LOG_NOTICE, "%s %s %s",
					    "ERROR - byte value greater",
					    "than 255 in PORT command",
					    "- terminating session");
					exit(EX_DATAERR);
				}
			} else if (ch == ',') {
				byte_number += 1;
				if (byte_number < 6)
					values[byte_number] = 0;
				else {
					syslog(LOG_NOTICE, "%s %s",
					    "too many byte values in PORT",
					    "command - terminating session");
					exit(EX_DATAERR);
				}
			}
			tailptr += 1;
		}

		if (byte_number != 5) {
			syslog(LOG_NOTICE, "Too few byte values in %s",
			    "PORT command - session terminated");
			exit(EX_DATAERR);
		}
		
		client_listen_sa.sin_family = AF_INET;
		client_listen_sa.sin_addr.s_addr = htonl((values[0] << 24) 
		    | (values[1] << 16) | (values[2] <<  8) 
		    | (values[3] <<  0));

		client_listen_sa.sin_port = htons((values[4] << 8) 
		    | values[5]);
		debuglog(1,"client wants us to use %u.%u.%u.%u:%u\n",
		    values[0], values[1], values[2], values[3],
		    (values[4] << 8) | values[5]);
		
		/*
		 * Configure our own listen socket and tell the server about it
		 */

		new_dataconn(1);
		
		connection_mode = PORT_MODE;
		
		debuglog(1,"we want server to use %s:%u\n",
		    inet_ntoa(server->sa.sin_addr),
		    ntohs(server_listen_sa.sin_port));	 
		
		snprintf(tbuf, sizeof(tbuf), "PORT %u,%u,%u,%u,%u,%u\r\n",
			 ((u_char *)&server->sa.sin_addr.s_addr)[0],
			 ((u_char *)&server->sa.sin_addr.s_addr)[1],
			 ((u_char *)&server->sa.sin_addr.s_addr)[2],
			 ((u_char *)&server->sa.sin_addr.s_addr)[3],
			 ((u_char *)&server_listen_sa.sin_port)[0],
			 ((u_char *)&server_listen_sa.sin_port)[1]);
		
		debuglog(1,"to server(modified):  %s",tbuf);
		
		sendbuf = tbuf;
	} else
		sendbuf = client->line_buffer;
	
	/* 
	 *send our (possibly modified) control command in sendbuf
	 * on it's way to the server
	 */
	if (sendbuf != NULL) {
		j = 0;
		i = strlen(sendbuf);
		do {
			rv = send(server->fd, sendbuf + j, i - j, 0);
			if (rv == -1 && errno != EAGAIN && errno != EINTR)
				break;
			else if (rv != -1)
				j += rv;
		} while (j >= 0 && j < i);
	}
}

void
do_server_reply(struct csiob *server, struct csiob *client)
{
	int code, i, j, rv;
	struct in_addr *iap;
	char tbuf[100];
	char *sendbuf;
	
	log_control_command((char *)server->line_buffer, 0);
	
	if (strlen((char *)server->line_buffer) > 512) {
		/*
		 * someone's playing games. Have a cow in the syslogs and
		 * exit - we don't pass this on for fear of hurting
		 * our other end, which might be poorly implemented.
		 */
		syslog(LOG_NOTICE, "Long (> 512 bytes) ftp control reply");
		exit(EX_DATAERR);
	}
	
	/*
	 * Watch out for "227 Entering Passive Mode ..." replies
	 */
	
	code = atoi((char *)server->line_buffer);
	if (code <= 0 || code > 999) {
		syslog(LOG_INFO,"invalid server reply code %d", code);
		exit(EX_DATAERR);
	}
	if (code == 227 && !NatMode) {
		u_char ch;
		u_char *tailptr;
		int byte_number;
		unsigned int values[6];
		
		debuglog(1, "Got a PASV reply\n");
		debuglog(1, "{%s}\n",(char *)server->line_buffer);
		
		tailptr = strchr((char *)server->line_buffer,'(');
		if (tailptr == NULL) {
			syslog(LOG_NOTICE, "malformed 227 reply");
			exit(EX_DATAERR);
		}
		
		tailptr += 1;	/* Move past the open-parentheses */
		byte_number = 0;
		values[0] = 0;
		while ((ch = *tailptr) == ',' || isdigit(ch)) {
			if (isdigit(ch)) {
				values[byte_number] = values[byte_number]
				    * 10 + ch - '0';
				if (values[byte_number] > 255) {
					syslog(LOG_NOTICE,
					    "malformed 227 reply");
					exit(EX_DATAERR);
				}
			} else if (ch == ',') {
				byte_number += 1;
				if (byte_number < 6) {
					values[byte_number] = 0;
				} else {
					syslog(LOG_NOTICE,
					    "malformed 227 reply");
					exit(EX_DATAERR);
				}
			}
			tailptr += 1;
		}
		
		/*
		 * The PASV reply should be terminated by a closing
		 * parentheses.
		 */
		
		if (ch != ')') {
			syslog(LOG_INFO,"malformed 227 reply, junk at end");
			exit(EX_DATAERR);
		}

		/* we need the righr number of bytes for ipv4 and port here */
		
		if (byte_number != 5) {
			syslog(LOG_NOTICE,
			    "malformed 227 reply, missing bytes");
			exit(EX_DATAERR);
		}
		
		server_listen_sa.sin_family = AF_INET;
		server_listen_sa.sin_addr.s_addr = htonl(
			(values[0] << 24) |
			(values[1] << 16) |
			(values[2] <<  8) |
			(values[3] <<  0)
			);
		server_listen_sa.sin_port = htons((values[4] << 8)
						  | values[5]);

		debuglog(1,"server wants us to use %s:%u\n",
			inet_ntoa(server_listen_sa.sin_addr),
			(values[4] << 8) | values[5]);
		
		new_dataconn(0);
		
		connection_mode = PASV_MODE;
		
		iap = &(server->sa.sin_addr);
		
		debuglog(1,"we want client to use %s:%u\n", inet_ntoa(*iap),
		    (((u_char *)&client_listen_sa.sin_port)[0] << 8)
			| ((u_char *)&client_listen_sa.sin_port)[1]);
		
		snprintf(tbuf, sizeof(tbuf),
		    "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u\r\n",
		    ((u_char *)iap)[0], ((u_char *)iap)[1],
		    ((u_char *)iap)[2], ((u_char *)iap)[3],
		    ((u_char *)&client_listen_sa.sin_port)[0],
		    ((u_char *)&client_listen_sa.sin_port)[1]);
		
		debuglog(1, "to client(modified):  %s\n",tbuf);
		
		sendbuf = tbuf;
	} else 
		sendbuf = server->line_buffer;

	/* 
	 *send our (possibly modified) control command in sendbuf
	 * on it's way to the client
	 */

	j = 0;
	i = strlen(sendbuf);
	do {
		rv = send(client->fd, sendbuf + j, i - j, 0);
		if (rv == -1 && errno != EAGAIN && errno != EINTR)
			break;
		else if (rv != -1)
			j += rv;
	} while (j >= 0 && j < i);

}

int
main(int argc, char **argv)
{
	struct csiob client_iob, server_iob;
	struct timeval tv;
	long timeout_seconds = 0;
	struct sigaction new_sa, old_sa;
	int sval, ch, salen, flags, i; 
	int use_tcpwrapper = 0;
	int one = 1;

	while ((ch = getopt(argc, argv, argstr)) != -1) {
		switch (ch) {
		case 'A':
			AnonFtpOnly = 1; /* restrict to anon usernames only */
			break;
		case 'w':
			use_tcpwrapper = 1; /* do the libwrap thing */
			break;
		case 'V':
			Verbose = 1;
			break;
		case 't':
			timeout_seconds = atoi(optarg);
			break;
		case 'D':
			Debug_Level = atoi(optarg);
			break;
		case 'r':
			Use_Rdns = 1; /* look up hostnames */
			break;
		case 'n':
			NatMode = 1; /* pass all passives, we're using NAT */
			break;
		case 'm':
			min_port = atoi(optarg);
			if (min_port < 0 || min_port > USHRT_MAX)
				usage();
			break;
		case 'M':
			max_port = atoi(optarg);
			if (min_port < 0 || min_port > USHRT_MAX)
				usage();
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;
	

	if (max_port < min_port) 
		usage();

	openlog(__progname, LOG_NDELAY|LOG_PID, LOG_DAEMON);
	
	setlinebuf(stdout);
	setlinebuf(stderr);
	
	memset(&client_iob, 0, sizeof(client_iob));
	memset(&server_iob, 0, sizeof(server_iob));
	
	if (get_proxy_env(0, &real_server_sa, &client_iob.sa) == -1) 
		exit(EX_PROTOCOL);
	
	/*
	 * We check_host after get_proxy_env so that checks are done 
	 * against the original destination endpoint, not the endpoint
	 * of our side of the rdr. This allows the use of tcpwrapper
	 * rules to restrict destinations as well as sources of connections
	 * for ftp.
	 */

	if (Use_Rdns) 
		flags = NI_NUMERICHOST | NI_NUMERICSERV;
	else 
		flags = 0;

	i = getnameinfo((struct sockaddr *)&client_iob.sa,
	    sizeof(client_iob.sa), ClientName, sizeof(ClientName), NULL, 0,
	    flags);

	if (i == -1) {
		syslog (LOG_ERR, "getnameinfo failed (%m)");
		exit(EX_OSERR);
	}	

	i = getnameinfo((struct sockaddr *)&real_server_sa, 
	    sizeof(real_server_sa), RealServerName, sizeof(RealServerName),
	    NULL, 0, flags);

	if (i == -1) {
		syslog (LOG_ERR, "getnameinfo failed (%m)");
		exit(EX_OSERR);
	}	

	if (use_tcpwrapper && !check_host(&client_iob.sa, &real_server_sa))
		exit(EX_NOPERM);

	client_iob.fd = 0;
	
	
	/*  Check to see if we have a timeout defined, if so, 
	 * set a timeout for this select call to that value, so 
	 * we may time out if don't see any data in timeout
	 * seconds. 
	 */
	
	tv.tv_sec = timeout_seconds;
	tv.tv_usec = 0;	
	
	timeout_seconds=tv.tv_sec;
	
	
	debuglog(1, "client is %s:%u\n", ClientName,
	    ntohs(client_iob.sa.sin_port));
	
	debuglog(1, "target server is %s:%u\n", RealServerName,
	    ntohs(real_server_sa.sin_port));
	
	server_iob.fd = get_backchannel_socket(SOCK_STREAM, min_port, max_port,
	    -1,	1, &server_iob.sa);
		
	if (connect(server_iob.fd, (struct sockaddr *)&real_server_sa,
	    sizeof(real_server_sa)) != 0) {
		syslog(LOG_INFO, "Can't connect to %s:%u (%m)", RealServerName,
		    ntohs(real_server_sa.sin_port));
		exit(EX_NOHOST);
	}
	
	/*
	 * Now that we are connected to the real server, get the name
	 * of our end of the server socket so we know our IP address
	 * from the real server's perspective.  
	 */

	i = getnameinfo((struct sockaddr *)&client_iob.sa, 
	    sizeof(client_iob.sa), OurName, sizeof(OurName), NULL, 0, flags);
	
	salen = sizeof(server_iob.sa);
	getsockname(server_iob.fd, (struct sockaddr *)&server_iob.sa, &salen);
	
	debuglog(1, "our end of socket to server is %s:%u\n", OurName,
	    ntohs(server_iob.sa.sin_port));
	
	/* ignore sigpipe */
	
	new_sa.sa_handler = SIG_IGN;
	(void)sigemptyset(&new_sa.sa_mask);
	new_sa.sa_flags = SA_RESTART;

	if (sigaction(SIGPIPE, &new_sa, &old_sa) != 0) {
		syslog(LOG_ERR,"sigaction failed (%m)");
		exit(EX_OSERR);
	}
	
	if (setsockopt(client_iob.fd, SOL_SOCKET, SO_OOBINLINE, (char *)&one,
	    sizeof(one)) == -1) {
		syslog(LOG_NOTICE,"Can't set SO_OOBINLINE (%m) - exiting");
		exit(EX_OSERR);
	}
		    
	client_iob.line_buffer_size = STARTBUFSIZE;
	client_iob.line_buffer = malloc(client_iob.line_buffer_size);
	client_iob.io_buffer_size = STARTBUFSIZE;
	client_iob.io_buffer = malloc(client_iob.io_buffer_size);
	client_iob.next_byte = 0;
	client_iob.io_buffer_len = 0;
	client_iob.alive = 1;
	client_iob.who = "client";
	client_iob.send_oob_flags = 0;
	client_iob.real_sa = client_iob.sa;
	
	
	server_iob.line_buffer_size = STARTBUFSIZE;
	server_iob.line_buffer = malloc(server_iob.line_buffer_size);
	server_iob.io_buffer_size = STARTBUFSIZE; 
	server_iob.io_buffer = malloc(server_iob.io_buffer_size);
	server_iob.next_byte = 0;
	server_iob.io_buffer_len = 0;
	server_iob.alive = 1;
	server_iob.who = "server";
	server_iob.send_oob_flags = MSG_OOB;
	server_iob.real_sa = real_server_sa;

	if (client_iob.line_buffer == NULL  ||  client_iob.io_buffer == NULL
	     || server_iob.line_buffer == NULL || server_iob.io_buffer == NULL)
	    {	
		syslog (LOG_NOTICE,  "Insufficient memory (malloc failed)");
		exit(EX_UNAVAILABLE);
	    }
	
	
	while (client_iob.alive || server_iob.alive) {
		fd_set fds;

		/*
		 * we use a static fd_set here, any individual
		 * instance of this program can't possibly have too
		 * many open files for FD_SETSIZE - we deal here only
		 * with one ftp connection
		 */
		
		debuglog(3,"client is %s, server is %s\n",
			client_iob.alive ? "alive" : "dead",
			server_iob.alive ? "alive" : "dead");

		FD_ZERO(&fds);
		
		if (client_iob.alive && telnet_getline(&client_iob, 
		    &server_iob)) {
			debuglog(3, "client line buffer is \"%s\"\n",
			    (char *)client_iob.line_buffer);
			if (client_iob.line_buffer[0] != '\0') 
				do_client_cmd(&client_iob, &server_iob);
		} else if (server_iob.alive && telnet_getline(&server_iob,
		    &client_iob)) {
			debuglog(3,"server line buffer is \"%s\"\n",
			    (char *)server_iob.line_buffer);
			if (server_iob.line_buffer[0] != '\0') 
				do_server_reply(&server_iob, &client_iob);
		} else {
			if (client_iob.alive) {
				FD_SET(client_iob.fd, &fds);
				if (client_listen_socket >= 0) 
					FD_SET(client_listen_socket, &fds);
				if (client_data_socket >= 0)
					FD_SET(client_data_socket, &fds);
			}
			if (server_iob.alive) {
				FD_SET(server_iob.fd, &fds); 
				if (server_listen_socket >= 0)
					FD_SET(server_listen_socket, &fds);
				if (server_data_socket >= 0) 
					FD_SET(server_data_socket, &fds);
			}
			
			tv.tv_sec = timeout_seconds;
			tv.tv_usec = 0;	

		doselect:
			switch (sval = select(FD_SETSIZE, &fds, NULL, NULL,
			    (tv.tv_sec == 0) ? NULL : &tv)) {
			case 0:
				/* 
				 * This proxy has timed out. Expire it 
				 * quietly with an obituary in the syslogs 
				 * for any passing mourners.
				 */
				syslog(LOG_INFO, 
				   "timeout, no data for %ld seconds",
				   timeout_seconds);
				exit(EX_OK); 
				break;
				
			case -1:
				if (errno == EINTR  || errno == EAGAIN)
					goto doselect;
				syslog(LOG_NOTICE,
				    "select failed (%m) - exiting");
				exit(EX_OSERR);
				break;
				
			default:
				if (client_data_socket >= 0 &&
				    FD_ISSET(client_data_socket,&fds)) {
					int rval;
					debuglog(3,"xfer client to server\n");
					rval = xfer_data("client to server",
					    client_data_socket,
					    server_data_socket,
					    client_iob.sa.sin_addr,
					    real_server_sa.sin_addr);
					if (rval <= 0) {
						shutdown(client_data_socket,2);
						close(client_data_socket);
						client_data_socket = -1;
						shutdown(server_data_socket,2);
						close(server_data_socket);
						server_data_socket = -1;
						show_xfer_stats();
					} else 
						client_data_bytes += rval;
				}
				if (server_data_socket >= 0 &&
				    FD_ISSET(server_data_socket,&fds)) {
					int rval;
					
					debuglog(3,"xfer server to client\n");
					rval = xfer_data("server to client",
					    server_data_socket,
					    client_data_socket,
					    real_server_sa.sin_addr,
					    client_iob.sa.sin_addr);
					if (rval <= 0) {
						shutdown(client_data_socket,2);
						close(client_data_socket);
						client_data_socket = -1;
						shutdown(server_data_socket,2);
						close(server_data_socket);
						server_data_socket = -1;
						show_xfer_stats();
					} else 
						server_data_bytes += rval;
				}
				if (server_listen_socket >= 0 &&
				    FD_ISSET(server_listen_socket,&fds)) {
					struct sockaddr_in listen_sa;
					
					/*
					 * We are about to accept a 
					 * connection from the server.
					 * This is a PORT or EPRT data
					 * connection.
					 */
					
					debuglog(2,
					    "server listen socket ready\n");
					
					if (server_data_socket >= 0) {
						shutdown(server_data_socket,2);
						close(server_data_socket);
						server_data_socket = -1;
					}
					if (client_data_socket >= 0) {
						shutdown(client_data_socket,2);
						close(client_data_socket);
						client_data_socket = -1;
					}
					salen = sizeof(listen_sa);
					server_data_socket =
					    accept(server_listen_socket,
					    (struct sockaddr *)&listen_sa,
					    &salen);
					if (server_data_socket < 0) {
						syslog(LOG_NOTICE,
						    "accept failed (%m)");
						exit(EX_OSERR);
					}
					close(server_listen_socket);
					server_listen_socket = -1;
					client_data_socket = socket(AF_INET,
				    	    SOCK_STREAM, 0);
					salen = 1;

					listen_sa.sin_family = AF_INET;
					
					bzero(&listen_sa.sin_addr,
					    sizeof(struct in_addr));
					
					debuglog(2,"setting sin_addr to %s\n",
					    inet_ntoa(client_iob.sa.sin_addr));
					
					listen_sa.sin_port = htons(20);
					salen = 1;
					setsockopt(client_data_socket,
					    SOL_SOCKET, SO_REUSEADDR, &salen,
					    sizeof(salen));
					if (bind(client_data_socket, 
					     (struct sockaddr *)&listen_sa,
					     sizeof(listen_sa)) < 0) {
						syslog(LOG_NOTICE,
						    "bind to 20 failed (%m)");
						exit(EX_OSERR);
					}
					
					if (connect(client_data_socket,
					    (struct sockaddr *)
					    &client_listen_sa,
					    sizeof(client_listen_sa)) != 0) {
						syslog(LOG_INFO,
						   "can't connect (%m)");
						exit(EX_NOHOST);
					}
					client_data_bytes = 0;
					server_data_bytes = 0;
					xfer_start_time = wallclock_time();
				}
				if (client_listen_socket >= 0 &&
				    FD_ISSET(client_listen_socket,&fds)) {
					struct sockaddr_in listen_sa;
					int salen;
					
					/*
					 * We are about to accept a
					 * connection from the client.
					 * This is a PASV data
					 * connection. 
					 */
					
					debuglog(2,
					    "client listen socket ready\n");
					
					if (server_data_socket >= 0) {
						shutdown(server_data_socket,2);
						close(server_data_socket);
						server_data_socket = -1;
					}
					if (client_data_socket >= 0) {
						shutdown(client_data_socket,2);
						close(client_data_socket);
						client_data_socket = -1;
					}
					salen = sizeof(listen_sa);
					client_data_socket = 
					    accept(client_listen_socket,
					    (struct sockaddr *)&listen_sa,
					    &salen);
					if (client_data_socket < 0) {
						syslog(LOG_NOTICE, 
						   "accept failed (%m)");
						exit(EX_OSERR);
					}
					
					close(client_listen_socket);
					client_listen_socket = -1;
					memset(&listen_sa, 0,
					    sizeof(listen_sa));
					server_data_socket =
					    get_backchannel_socket(SOCK_STREAM,
					    min_port, max_port, -1, 1,
					    &listen_sa);
					if (server_data_socket < 0) {
						syslog(LOG_NOTICE,
						    "backchannel failed (%m)");
						exit(EX_OSERR);
					}
					if (connect(server_data_socket,
					    (struct sockaddr *)
					    &server_listen_sa,
					    sizeof(server_listen_sa)) != 0) {
						syslog(LOG_NOTICE,
						    "connect failed (%m)");
						exit(EX_NOHOST);
					}
					client_data_bytes = 0;
					server_data_bytes = 0;
					xfer_start_time = wallclock_time();
				}
				if (client_iob.alive &&
				    FD_ISSET(client_iob.fd,&fds)) {
					client_iob.data_available = 1;
				}
				
				if (server_iob.alive &&
				    FD_ISSET(server_iob.fd,&fds)) {
					server_iob.data_available = 1;
				}
				
			}
		}
		
		if (client_iob.got_eof) {
			shutdown(server_iob.fd,1);
			shutdown(client_iob.fd,0);
			client_iob.got_eof = 0;
			client_iob.alive = 0;
		}
		
		if (server_iob.got_eof) {
			shutdown(client_iob.fd,1);
			shutdown(server_iob.fd,0);
			server_iob.got_eof = 0;
			server_iob.alive = 0;
		}
		
	}
	exit(EX_OK);
}
