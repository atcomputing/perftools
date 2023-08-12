/* usecpu.c
**
** Force a well-defined pattern of CPU utilization.
**
** Usage: usecpu [C] [P%]
**          C - number of CPU seconds to consume in total (default: infinite)
**          P - percentage of forced consumption (default: 100%)
**
** Example:	usecpu 30 25%
**
**		Forces 25% of CPU utilization until 30 seconds of CPU time
**		is consumed (i.e. lasting two minutes of wall clock time).
**
** ==========================================================================
** Author:       Gerlof Langeveld
** Copyright (C) 2008  AT Computing
** Modified:     2020  AT Computing - added percentage
** ==========================================================================
** This file is free software.  You can redistribute it and/or modify
** it under the terms of the GNU General Public License (GPL); either
** version 3, or (at your option) any later version.
*/

#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	EVER		;;

unsigned long long	totalcpusec=9999999999, cpuperc=100, cpumsec;

struct timespec		curtime, pretime, sleeptime;
struct itimerval	itv;

/*
** signal handler for SIGVTARLM signal that is triggered
** by CPU consumption
*/
void
checkutil(int sig)
{
	unsigned long long	mcur, mpre, mdif;

	// oneshot number of CPU seconds defined?
	if (cpuperc == 100)
		exit(0);

	// keep track of real time to see if we have to sleep
	// a while to fullfill the required CPU percentage
	pretime = curtime;
	clock_gettime(CLOCK_REALTIME, &curtime);

	mcur = (unsigned long long)curtime.tv_sec * 1000000 + 
	                          (curtime.tv_nsec / 1000);

	mpre = (unsigned long long)pretime.tv_sec * 1000000 +
	                          (pretime.tv_nsec / 1000);

	mdif = mcur - mpre;

        if (mdif < 1000000) {
		sleeptime.tv_nsec = (1000000 - mdif) * 1000;
		nanosleep(&sleeptime, (struct timespec *)0);
	}

	// in case of explicit CPU percentage:
	// - accumulate total CPU consumption so far
	// - verify if we have exceeded total CPU consumption
	cpumsec += 10000*cpuperc;

	if (cpumsec >= totalcpusec * 1000000)
		exit(0);

	clock_gettime(CLOCK_REALTIME, &curtime);
}

int
main(int argc, char *argv[])
{
	int			i;
	unsigned long		val;
	char			*p;

	// check all arguments
	for (i=1; i < argc; i++) {
		val = strtol(argv[i], &p, 10);

		switch (*p) {
		   case '\0':		// CPU seconds to use
			totalcpusec = val;
			break;
		   case '%':		// CPU percentage to force
			if (val <= 100) {
				cpuperc = val;
				break;
			}
		   default:
			fprintf(stderr, "Usage: usecpu [cpusec] [cpuperc%]\n");
			fprintf(stderr, "     cpusec   - number of CPU seconds "
			                "to consume in total "
			                "(default: infinite)\n");
			fprintf(stderr, "     cpuperc%% - percentage of CPU "
			                "utilization (default: 100%, "
					"max 100%)\n");
			exit(1);
		}
	}

	// define signal handler
	(void) signal(SIGVTALRM, checkutil);

	// oneshot signal with 100% consumption?
	if (cpuperc == 100) {
		if (totalcpusec > 0) {
			itv.it_value.tv_sec = totalcpusec;
			(void) setitimer(ITIMER_VIRTUAL, &itv,
						(struct itimerval *)0);
		}
	} else {
		// multishot signal with less than 100% consumption?
		itv.it_interval.tv_sec  = 0;
		itv.it_interval.tv_usec = 10000*cpuperc;

		itv.it_value.tv_sec     = 0;
		itv.it_value.tv_usec    = 10000*cpuperc;

		(void) setitimer(ITIMER_VIRTUAL, &itv, (struct itimerval *)0);
	}

	clock_gettime(CLOCK_REALTIME, &curtime);

	// start wasting cpu-cyles
	for (EVER);

	return 0;
}
