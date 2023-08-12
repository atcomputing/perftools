/* attract.c -- AT Computing Traffic Achievement Tool
**
** Force a well-defined network load.
**
** Use this program to measure the transfer rate between two systems
** via the TCP or UDP transport layer.
**
** Server activation:
** ------------------
**   Start this program once as server-process on the target system:
**
**       attract -s [-P port]
**
**   Attract switches itself to the backbround and opens its standard
**   port number (unless an alternative port number has been specified
**   with the -P flag). Preferably a port is opened which allows IPv4
**   and IPv6 communictaion, unless IPv6 is not supported by the 
**   target system.
**
**   As soon as a client connection is accepted, attract spawns a child
**   process to handle that connection. The attract server can handle
**   more measurements simultaneously.
**
**
** Client activation:
** ------------------
**   Once the attract-server is running, an attract-client can be started:
**
**       attract [-v ipvers] [-p prot] [-d direct] [-l len] [-c cnt|-t sec]
**               [-P portnr]  host
**
**   The client-attract connects to the server and starts measuring the
**   peer-to-peer capactity by transmitting (and receiving in case of
**   bidirectional transfer) data packets.
**
**   The following flags can be used to influence the transfer
**   characteristics:
**
**     flag value     default            description
**     -------------------------------------------------------------------
**      -v  4 | 6     6 (if possible)    IP-version (IPv4 or IPv6)
**      -p  t | u     t                  transport protocol (tcp or udp)
**      -d  u | b     u                  direction of transfer
**                                       (unidirectional or bidirectional)
**      -l  length    512                packet length
**      -c  number    0                  number of packets
**      -t  seconds   10                 transfer time in seconds
**      -P  portnr    31432              port number for communication;
**
**   Default:
**      Measure the throughput per second while transferring packets
**      of 512 bytes in one direction via TCP (preferably IPv6) during 
**      an elapsed time of 10 seconds.
**
** ==========================================================================
** Author: 	Gerlof Langeveld - AT Computing - Nijmegen - The Netherlands 
** Date:   	February 1999
** Modified:	IPv6 support, variable port number (April 2003)
** Modified:	Enlarge counters (April 2014)
** ==========================================================================
** This file is free software.  You can redistribute it and/or modify
** it under the terms of the GNU General Public License (GPL); either
** version 3, or (at your option) any later version.
*/

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/param.h>
#include <sys/times.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>

#define	MYPORT	"31432"
#define	MAXLEN	65636
#define	UDPTOUT	5		/* timeout (seconds) for UDP-receive */
#define	EVER	;;


/*
** argument handling
*/
char *usage[]= {
	"Usage client: %s [-v ipvers] [-p prot] [-d direct] [-l len] "
	"[-c cnt | -t sec] [-P portnum] [-r]  host\n",
	"\t-v\t4 = ipv4 / 6 = ipv6 (default if available)\n",
	"\t-p\tt (for tcp = default) / u (for udp)\n",
	"\t-d\tu (uni = default) / b (bi)\n",
	"\t-l\tpacket-length            (default 512)\n",
	"\t-c\tpacket-count             (default   0)\n",
	"\t-t\ttransfer-time in seconds (default  10)\n",
	"\t-P\talternative port number  (default " MYPORT ")\n",
	"\t-r\traw output required (easy parsing)\n",
	"\nUsage server: %s -s [-P portnum]\n",
	"\t-s\tstart as a server\n",
	"\t-P\talternative port number (default " MYPORT ")\n",
	(char *) 0,
};

extern char     *optarg;
extern int      optind, opterr;

char		ipvers='?', prot='t', direct='u', rawout=0,
		*hostname, *myport = MYPORT;

unsigned int	mesnum=0, meslen=512, mestime=10;

/*
** timeout handling for UDP-receive
*/
int		timedout;	/* boolean: has timeout occurred    */
long long	numtout;	/* number of times timeout occurred */

/*
** function prototypes
*/
static void  genserver(void);
static void  handlecon(int, struct addrinfo *);
static int   getmes(char, int, int, char *, int, struct sockaddr *, socklen_t);
static void  putmes(char, int, int, char *, int, struct sockaddr *, socklen_t);
static void  wakeupcall(int);
static int   gettcpsock(int, char *, struct addrinfo **, char);
static int   getudpsock(int, char *, char *, struct addrinfo **, char);
static int   numhost(char *);

/***************************************************************************/

int
main(int argc, char **argv)
{
	int			tcpsock, udpsock=0;
	long			localhz=sysconf(_SC_CLK_TCK);
	char			buf[MAXLEN+1], udpport[64], serverflag=0;
	int			i, c, len, factor, proceed=1;
	long long		cnt=0, numrcv;
	clock_t			begtime, endtime, realticks, hogclnt, hogserv;
	struct tms		begtms, endtms;
	struct addrinfo		*sockinfo;

	/*
	** verify command line arguments
	*/
	if (argc < 1) {
		for (i=0; usage[i]; i++)
			fprintf(stderr, usage[i], argv[0]);
		exit(1);
	}

	while ( (c = getopt(argc, argv, "srv:p:d:l:c:t:P:")) != EOF) {
		/*
		** react on flag
		*/
		switch (c) {
		   case 's':
			serverflag=1;
                        break;

		   case 'r':
			rawout = 1;
			break;

		   case 'v':
                        ipvers = *optarg;

			if (ipvers != '4' && ipvers != '6') {
				fprintf(stderr, "Wrong value for version\n");
				exit(1);
			}
                        break;

		   case 'p':
                        prot   = *optarg;

			if (prot != 't' && prot != 'u') {
				fprintf(stderr, "Wrong value for protocol\n");
				exit(1);
			}
                        break;

		   case 'd':
                        direct = *optarg;

			if (direct != 'u' && direct != 'b') {
				fprintf(stderr, "Wrong value for direction\n");
				exit(1);
			}
                        break;

		   case 'l':
			if ( (meslen = atoi(optarg)) <= 0) {
				fprintf(stderr, "Wrong value for length\n");
				exit(1);
			}

			if (meslen > MAXLEN) {
				fprintf(stderr,
					"Maximum length is %d\n", MAXLEN);
				exit(1);
			}
                        break;

		   case 'c':
			if ( (mesnum = atoi(optarg)) <= 0) {
				fprintf(stderr, "Wrong value for count\n");
				exit(1);
			}
                        break;

		   case 't':
			if ( (mestime = atoi(optarg)) <= 0) {
				fprintf(stderr, "Wrong value for timeout\n");
				exit(1);
			}
                        break;

		   case 'P':
			myport = optarg;
			break;

		   default:
			for (i=0; usage[i]; i++)
				fprintf(stderr, usage[i], argv[0]);
			exit(1);
		}
	}

	/*
	** server behaviour required?
	*/
	if (serverflag)
		genserver();	/* and never return from here */

/**************************************************************************/
/*                             C L I E N T                                */
/**************************************************************************/

	/*
	** obtain the hostname
	*/
        if ( (argc - optind) < 1 ) {
		for (i=0; usage[i]; i++)
			fprintf(stderr, usage[i], argv[0]);
                exit(1);
        }

        hostname = argv[optind++];

	switch (ipvers) {
	   case '4':
		if ((tcpsock=gettcpsock(AF_INET, hostname, &sockinfo, 1))==-1)
			exit(1);
		break;

	   case '6':
		if ((tcpsock=gettcpsock(AF_INET6, hostname, &sockinfo, 1))==-1)
			exit(1);
		break;

	   default:
		if ((tcpsock=gettcpsock(AF_INET6, hostname, &sockinfo, 0)) >= 0) {
			ipvers = '6';
			break;
		}

		if ((tcpsock=gettcpsock(AF_INET, hostname, &sockinfo, 1)) >= 0) {
			ipvers = '4';
			break;
		}

		exit(1);
	}

	/*
	** pass control-info to the server;
	** keep in mind that the server can run on a different CPU-type
	** (Little/Big Endian), so pass info in ASCII:
	**    -	ipversion (4 for ipv4, 6 for ipv6),
	**    -	protocol  (t for tcp,  u for udp),
	**    -	direction (u for uni,  b for bi),
	**    -	packet-length in bytes.
	*/
	sprintf(buf, "%c %c %c %d\n", ipvers, prot, direct, meslen);

	if ( write(tcpsock, buf, strlen(buf)+1) < 0) {
		perror("c: write control-info");
		exit(1);
	}

	/*
	** wait until a control-packet is transmitted back;
	** in case of UDP-transmission, it holds the UDP-port in ASCII
	*/
	if ( read(tcpsock, udpport, sizeof udpport) < 0) {
		perror("c: read control-info");
		exit(1);
	}

	/*
	** open a separate UDP-connection, if UDP-transport required
	*/
	if (prot == 'u') {
		freeaddrinfo(sockinfo); /* remove TCP-socket info first */

		if ((udpsock = getudpsock(ipvers == '4' ? AF_INET : AF_INET6,
					hostname, udpport, &sockinfo, 1)) < 0)
			exit(3);
	}

	/*
	** prepare data-transport
	*/
	memset(buf, 'X', sizeof(buf));
	wakeupcall(0);	/* catch timer-signals from now on */

	begtime	= times(&begtms);	   	/* register start-time */
	endtime = begtime + mestime * localhz;  /* calculate wanted end-time */

	/*
	** data-transfer loop .....
	*/
	while (proceed) {
		cnt++;

		/*
		** Check if the transfer can be finished, either because
		** the required number of messages have been sent, or the
		** required time has passed.
		*/
		if (mesnum) {		/* counter-based transfer */
			if (cnt == mesnum) {
				proceed=0;
				buf[0] = 'E';	/* set EOT indicator */
			}
		} else {			/* time-based transfer */
			if ( (cnt & 0x1f) == 0 && times(&endtms) >= endtime) {
				proceed=0;
				buf[0] = 'E';	/* set EOT indicator */
			}
		}

		/*
		** send a packet
		*/
		putmes(prot, tcpsock, udpsock, buf, meslen,
				sockinfo->ai_addr, sockinfo->ai_addrlen);


		if (direct == 'u')
			continue;

		/*
		** receive a packet (only if bidirectional)
		*/
		getmes(prot, tcpsock, udpsock, buf, meslen,
				sockinfo->ai_addr, sockinfo->ai_addrlen);

		/* might have timed out in case of UDP packet loss */
	}

	/*
	** Transfer is finished .....
	**
	** Obtain from the server (via the TCP control-channel):
	**    -	number of received packets,
	**    -	number of seconds server timed out
	**    -	cpu-consumption in units of 10 milliseconds
	*/
	timedout = 0;
	alarm(UDPTOUT+5);

	if ( (len = read(tcpsock, buf, sizeof(buf))) <= 0) {
		if (timedout || len == 0) {
			fprintf(stderr, "No response from server\n");
			exit(1);
		}
		perror("c: read");
		exit(1);
	}
	sscanf(buf, "%lld %lld %ld", &numrcv, &numtout, &hogserv);

	/*
	** report statistics
	*/
	realticks = times(&endtms) - begtime - (numtout * localhz);

	if (realticks == 0)
			realticks = 1;

	hogclnt   = ((endtms.tms_utime - begtms.tms_utime)  +
		     (endtms.tms_stime - begtms.tms_stime)) * 100 / realticks;

	hogserv   = hogserv * localhz / realticks;

	if (direct == 'b')
		factor=2;
	else
		factor=1;

	if (rawout) {
		printf("%c %-3s %s %6d %8lld %4ld.%02ld %9lld %9lld %3lld "
		       "%4ld.%02ld %4ld.%02ld\n",
			sockinfo->ai_family == AF_INET6 ? '6' : '4',
			direct == 'u' ? "uni" : "bi", 
			prot   == 'u' ? "udp" : "tcp",
			meslen, cnt,
			realticks / localhz,
			((realticks % localhz) * 100) / localhz,
			(((numrcv * meslen * factor) / realticks)
						* localhz) / 1024,
			cnt-numrcv, (cnt-numrcv) * 100 / cnt,
			hogclnt / 100, hogclnt % 100,
			hogserv / 100, hogserv % 100);
	} else {
		printf("%sdirectional transfer via %sv%c with size %d bytes:\n",
			direct == 'u' ? "Uni" : "Bi",
			prot   == 'u' ? "UDP" : "TCP",
			sockinfo->ai_family == AF_INET6 ? '6' : '4',
			meslen);

		printf("\t%lld packets in %.2f seconds = %lld K/s "
		       "(%lld packets lost = %lld%%)\n",
			cnt, (float)realticks / localhz,
			(((numrcv * meslen * factor) / realticks)
						* localhz) / 1024,
			 cnt-numrcv,
			(cnt-numrcv) * 100 / cnt);

		printf("\thog-factor client: %.2f, hog-factor server: %.2f\n",
			(float)hogclnt / 100.0, (float)hogserv / 100.0);
	}

	return 0;
}

/*
** open a stream socket and connect to host with givben family
*/
static int
gettcpsock(int family, char *hostname, struct addrinfo **si, char verbose)
{
	int		sockfd, i;
	struct addrinfo	sockhints, *r;

	/*
	** determine IP-address of server
	*/
	memset(&sockhints, 0, sizeof sockhints);

	sockhints.ai_socktype	= SOCK_STREAM;
	sockhints.ai_family	= family;

#ifdef	AI_NUMERICHOST		/* for AIX */
	if (numhost(hostname))
		sockhints.ai_flags	= AI_NUMERICHOST;
#endif

	if ( (i = getaddrinfo(hostname, myport, &sockhints, si)) ) {
		if (verbose)
			fprintf(stderr, "c: host %s: %s\n",
				hostname, gai_strerror(i));
		return -1;
	}

	/*
	** open a TCP-connection as control-channel
	*/
	for (r=*si; r; r=r->ai_next) {
		if ( (sockfd = socket((*si)->ai_family,
			              (*si)->ai_socktype,
	               		      (*si)->ai_protocol)) < 0)
			continue;

		/*
		** connect to TCP-port of the server
		*/
		if ( connect(sockfd, (*si)->ai_addr,
		                     (*si)->ai_addrlen) == 0)
			return sockfd;

		close(sockfd);
	}

	freeaddrinfo(*si);

	if (verbose)
		perror("connect to TCP socket");

	return -1;
}

/*
** open a datagram socket
*/
static int
getudpsock(int family, char *host, char *port,
	   struct addrinfo **si,   char verbose)
{
	int		sockfd, i;
	struct addrinfo	sockhints, *r;

	memset(&sockhints, 0, sizeof sockhints);

	sockhints.ai_socktype	= SOCK_DGRAM;
	sockhints.ai_family	= family;

#ifdef	AI_NUMERICHOST		/* for AIX */
	if (numhost(hostname))
		sockhints.ai_flags	= AI_NUMERICHOST;
#endif

	if ( (i = getaddrinfo(hostname, port, &sockhints, si)) ) {
		if (verbose)
			fprintf(stderr, "c: host %s: %s\n",
					hostname, gai_strerror(i));
		return -1;
	}

	for (r=*si; r; r = r->ai_next) {
		if ( (sockfd = socket((*si)->ai_family,
		                      (*si)->ai_socktype,
			              (*si)->ai_protocol)) >= 0)
			return sockfd;
	}

	freeaddrinfo(*si);

	if (verbose)
		perror("connect to UDP socket");

	return -1;
}

/**************************************************************************/
/*                              S E R V E R                               */
/**************************************************************************/
static void
genserver(void)
{
	int		passock, comsock;
	socklen_t	namsz;
	struct addrinfo *sockinfo, sockhints;

	/*
	** daemonize this process ...
	*/
	signal(SIGHUP, SIG_IGN);

	if ( fork() )
		exit(0);		/* anyhow switch to background */

	setsid();

	if ( fork() )
		exit(0);

	wakeupcall(0);		/* catch timer-signal from now on */

	/*
	** create endpoint for the TCP control-channel for ipv6 (if possible)
	*/
	memset(&sockhints, 0, sizeof sockhints);

	sockhints.ai_flags	= AI_PASSIVE;
	sockhints.ai_family	= AF_INET6;
	sockhints.ai_socktype	= SOCK_STREAM;

	passock = -1;

	if ( getaddrinfo(NULL, myport, &sockhints, &sockinfo) == 0) {
		if ((passock = socket(sockinfo->ai_family,
		                      sockinfo->ai_socktype,
		                      sockinfo->ai_protocol)) >= 0) {
			/*
			** bind port-address to socket
			*/
			if (bind(passock, sockinfo->ai_addr,
			                  sockinfo->ai_addrlen) == 0) {
				/*
				** prepare listening on socket
				*/
				if (listen(passock, 5) < 0)
				{
					perror("s: listen ipv6");
					exit(1);
				}
			} else {
				perror("s: bind ipv6");
				exit(1);
			}
		}
	}

	/*
	** IPv6 socket failed?
	*/
	if (passock == -1) {
		/*
		** create endpoint for the TCP control-channel for ipv4
		*/
		memset(&sockhints, 0, sizeof sockhints);

		sockhints.ai_flags	= AI_PASSIVE;
		sockhints.ai_family	= AF_INET;
		sockhints.ai_socktype	= SOCK_STREAM;

		if ( getaddrinfo(NULL, myport, &sockhints, &sockinfo) == 0) {
			if ((passock = socket(sockinfo->ai_family,
			                      sockinfo->ai_socktype,
			                      sockinfo->ai_protocol)) >= 0) {
				/*
				** bind port-address to socket
				*/
				if (bind(passock, sockinfo->ai_addr,
				                  sockinfo->ai_addrlen) == 0) {
					/*
					** prepare listening on socket
					*/
					if (listen(passock, 5) < 0)
					{
						perror("s: listen ipv4");
						exit(1);
					}
				} else {
					perror("s: bind ipv4");
					exit(1);
				}
			}
		} else {
			perror("s: getaddrinfo ipv4");
			exit(1);
		}
	}

	/*
	** get rid of finished child-processes from now on (avoid zombies)
	*/
	signal(SIGCLD, SIG_IGN);

	/*
	** wait for incoming control-connection and
	** spawn a child-process to handle that connection
	*/
	for (EVER) {
		namsz = sockinfo->ai_addrlen;

		if ( (comsock = accept(passock, sockinfo->ai_addr, &namsz))<0) {
			perror("s: accept");
			exit(1);
		}

		/*
		** spawn new process to handle new client
		*/
		if ( fork() == 0 ) {
			close(passock);
			handlecon(comsock, sockinfo);
		}

		/*
		** parent continues awaiting new clients
		*/
		close(comsock);
	}
}

/*
** child-process handling a client connection
*/
static void
handlecon(int tcpsock, struct addrinfo *tcpinfo)
{
	int		udpsock=0, udpport=0;
	long long	targetlen, numrcv=0;
	long		localhz=sysconf(_SC_CLK_TCK);
	char		buf[MAXLEN+1] = { 0 };
	char		ctlinfo[32], ipvers, prot, direct;
	clock_t		cpuserv;
	struct tms	begtms, endtms;
	struct addrinfo	*sockinfo, sockhints;

	/*
	** receive control-info which will be passed as first packet
	** by the client
	*/
	if ( read(tcpsock, ctlinfo, sizeof(ctlinfo)) <= 0) {	
		close(tcpsock);
		exit(1);
	}

	/*
	** split up received control-info:
	**    -	ipversion (4 for ip4, 6 for ip6),
	**    -	protocol  (t for tcp, u for udp),
	**    -	direction (u for uni, b for bi),
	**    -	packet-length in bytes.
	*/
	sscanf(ctlinfo, "%c %c %c %lld", &ipvers, &prot, &direct, &targetlen);

	/*
	** search a free port if communication via UDP is required
	*/
	if (prot == 'u') {
		memset(&sockhints, 0, sizeof sockhints);

		sockhints.ai_socktype	= SOCK_DGRAM;
		sockhints.ai_flags	= AI_PASSIVE;

		switch (ipvers) {
		   case '4':
			sockhints.ai_family	= AF_INET;
			break;

		   case '6':
			sockhints.ai_family	= AF_INET6;
			break;
		}

		/*
		** trial-and-error loop to see which port is free
		*/
		for (udpport=atoi(myport); ; udpport++) {
			char	portname[64];

			sprintf(portname, "%d", udpport);

			if (getaddrinfo(NULL, portname, &sockhints, &sockinfo))
				continue;

			if ( (udpsock = socket(	sockinfo->ai_family,
						sockinfo->ai_socktype,
						sockinfo->ai_protocol)) < 0) {
				freeaddrinfo(sockinfo);
				continue;
			}

			/*
			** bind port address to socket
			*/
			if ( bind(udpsock, sockinfo->ai_addr,
			                   sockinfo->ai_addrlen) == 0)
				break;

			close(udpsock);
			freeaddrinfo(sockinfo);
		}
	}

	/*
	** tell the other side that communication is ready
	** and pass the UDP-port (only relevant if UDP-connection wanted)
	*/
	sprintf(buf, "%d", udpport);

	if ( write(tcpsock, buf, strlen(buf)+1) < 0) {
		perror("s: write");
		exit(1);
	}

	/*
	** take notice of my own CPU-consumption till now
	*/
	(void) times(&begtms);

	/*
	** data-transfer loop to measure throughput......
	*/
	for (EVER) {
		/*
		** Receive a packet
		*/
		if (getmes(prot, tcpsock, udpsock, buf, targetlen,
		           prot == 'u' ? sockinfo->ai_addr    : 0,
		           prot == 'u' ? sockinfo->ai_addrlen : 0))
			break;		/* UDP-timeout received */

		numrcv++;

		/*
		** only if bi-directional: send packet back
		*/
		if (direct == 'b')
			putmes(prot, tcpsock, udpsock, buf, targetlen,
			       prot == 'u' ? sockinfo->ai_addr    : 0,
			       prot == 'u' ? sockinfo->ai_addrlen : 0);

		/*
		** check packet contents: EOT-indicator (final packet)?
		*/
		if ( buf[0] == 'E' )
			break;
	}

	/*
	** take notice of my own CPU-consumption again and see
	** how many CPU-time is used during transfer
	*/
	(void) times(&endtms);

	cpuserv   = (endtms.tms_utime - begtms.tms_utime) +
		    (endtms.tms_stime - begtms.tms_stime);

	cpuserv   = cpuserv * 100 / localhz;	/* hundreds of seconds */

	/*
	** send to the client:
	**    -	number of received packets,
	**    -	number of seconds server timed out
	**    -	cpu-consumption in units of 10 milliseconds
	*/
	sprintf(buf, "%lld %lld %ld", numrcv, numtout*UDPTOUT, cpuserv);

	if ( write(tcpsock, buf, strlen(buf)+1) < 0) {
		perror("s: write");
		exit(1);
	}

	close(tcpsock);

	exit(0);
}

/*
** determine if a host is identified by a name or a numerical string
*/
static int
numhost(char *hp)
{
	register int nalpha=0;

	for (; *hp; hp++) {
		switch (*hp) {
		   case ':':
			return 1;

		   case '.':
			if (nalpha)
				return 0;
			break;

		   default:
	 		if (!isdigit(*hp))
				nalpha++;
		}
	}

	if (nalpha)
		return 0;
	else
		return 1;
}


/**************************************************************************/
/*                           G E N E R I C                                */
/**************************************************************************/
/*
** receive one packet
*/
static int
getmes(char prot, int tcpsock, int udpsock, char *p, int len,
				struct sockaddr *sockname, socklen_t addrlen)
{
	int rv;

	/*
	** handle a TCP-receive
	*/
	if (prot == 't') {
		/*
		** read exactly one packet of indicated length
		** (might arrive in smaller fragments)
		*/
		while (len) {
			if ( (rv = read(tcpsock, p, len)) <= 0) {
				close(tcpsock);
				exit(0);
			}
	
			len -= rv;
			p   += rv;
		}
	} else {
		/*
		** handle a UDP-receive
		*/
		timedout = 0;
		alarm(UDPTOUT);

		if ( (rv = recvfrom(udpsock, p, len, 0, sockname, &addrlen)) <0) {
			if (timedout) {
				numtout++;
				return(1);
			}

			perror("recvfrom");
			exit(1);
		}

		alarm(0);	/* reset timer */
	}
	return(0);
}

/*
** transmit one packet
*/
static void
putmes(char prot, int tcpsock, int udpsock, char *p, int len,
				struct sockaddr *sockname, socklen_t addrlen)
{
	/*
	** handle a TCP-transmit
	*/
	if (prot == 't') {
		if ( write(tcpsock, p, len) < 0) {
			perror("write");
			exit(1);
		}
	} else {
		/*
		** handle a UDP-transmit
		*/
		if ( sendto(udpsock, p, len, 0, sockname, addrlen) < 0) {
			perror("sendto");
			exit(1);
		}
	}
}

/*
** signal-catcher for alarm-signal
*/
static void
wakeupcall(int sig)
{
	timedout = 1;

#ifdef	linux
	sysv_signal(SIGALRM, wakeupcall); /* be prepared for next signal */
#else
	signal(SIGALRM, wakeupcall);      /* be prepared for next signal */
#endif
}
