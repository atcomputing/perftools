/* usecpu.c
**
** Force a well-defined pattern of CPU utilization.
**
** Usage: usecpu [C] [P%] [Nt]
**          C - number of CPU seconds to consume in total (default: infinite)
**          P - percentage of forced consumption (default: 100%)
**          N - number of parallel threads (default: 1)
**
** Example:	usecpu 30 25%
**
**		Forces 25% of CPU utilization until 30 seconds of CPU time
**		is consumed (i.e. lasting two minutes of wall clock time
**		when enough CPU capacity is available).
**
** Example:	usecpu 30 5t
**
**		Forces 5 threads to consume 30 seconds of CPU time in total.
**		On a system with at least 5 CPUs this might take only 6 seconds
**              of wall clock time.
**
** Example:	usecpu 100 10t 50%
**
**		Forces 10 threads to consume 100 seconds of CPU time in total
**              while each thread only uses 50% of CPU capacity.
**		On a system with at least 5 CPUs this probably takes 20 seconds
**              of wall clock time.
**
** ==========================================================================
** Author:       Gerlof Langeveld
** Copyright (C) 2008  AT Computing
** Modified:     2020  AT Computing - added percentage
** Redesigned:   2025  AT Computing - added multithreading
** ==========================================================================
** This file is free software.  You can redistribute it and/or modify
** it under the terms of the GNU General Public License (GPL); either
** version 3, or (at your option) any later version.
*/
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>


#define	MSLICE	100000	// wall clock slice in microseconds: 0.1 second
#define	EVER	;;

static unsigned long long	timespec2micro(struct timespec *);
static void 			*cpuwaste(void *);
static void			normalstop(int);

unsigned long long		numsecs=9999999999, cpuperc=100;

int
main(int argc, char *argv[])
{
	int			i, numthreads=1;
	unsigned long		val;
	char			*p;
	pthread_t		tid;
	struct rlimit 		rlim;

	// check all arguments
	//
	for (i=1; i < argc; i++) {
		val = strtol(argv[i], &p, 10);

		switch (*p) {
		   case '\0':		// CPU seconds to use
			numsecs = val;
			break;

		   case 't':		// number of threads
			if (argv[i] == p) {
				fprintf(stderr, "no value in front of 't'\n");
				exit(1);
			}

			if (val > 1)
				numthreads = val;
			break;

		   case '%':		// CPU percentage to force
			if (argv[i] == p) {
				fprintf(stderr, "no value in front of '%%'\n");
				exit(1);
			}

			if (val > 0 && val <= 100) {
				cpuperc = val;
				break;
			}
			// fall through
		   default:
			fprintf(stderr, "Usage: usecpu [cpusec] [cpuperc%%] [Nt]\n");
			fprintf(stderr, "     cpusec   - number of CPU seconds "
			                "to consume in total (default: infinite)\n");
			fprintf(stderr, "     cpuperc%% - percentage of CPU "
			                "utilization (default: 100%%, "
					"max 100%%)\n");
			fprintf(stderr, "           Nt - execute N threads in parallel "
					"(default: 1)\n");
			exit(1);
		}
	}

	// set the maximum allowed CPU consumption for this process
	// and catch the signal that is generated when this CPU consumption
	// is exceeded
	//
	(void) signal(SIGXCPU, normalstop);

	(void) getrlimit(RLIMIT_CPU, &rlim);
	rlim.rlim_cur = numsecs;
	(void) setrlimit(RLIMIT_CPU, &rlim);

	// start additional threads to waste CPU cycles
	//
	for (i=0; i < numthreads-1; i++)
		pthread_create(&tid, (pthread_attr_t *)0, cpuwaste, (void *) 0);

	// let main thread itself waste CPU cycles
	//
	cpuwaste(0);

	// in the meantime, wait until we (i.e. all threads) are killed by
	// the kernel when our maximum CPU limit has been reached

	return 0;	// never reached
}

// convert struct timespec to microseconds
//
static unsigned long long
timespec2micro(struct timespec *ts)
{
	return (unsigned long long)ts->tv_sec * 1000000 + (ts->tv_nsec / 1000);
}


// CPU consumer executed by each thread
//
static void *
cpuwaste(void *dummy)
{
	struct timespec		curcputime, begcputime,
				curwalltime, begwalltime,
				sleeptime;
	unsigned long long	mcurcpu,  mbegcpu,  mdifcpu,
				mcurwall, mbegwall, mdifwall,
				mwaste;

	////////////////////////////////////////////////////////
	// start wasting 100% of CPU cycles
	// in user mode
	//
	if (cpuperc == 100) {
		for (EVER)
			;;
	}

	////////////////////////////////////////////////////////
	// or start wasting a smaller percentage of CPU cycles
	//
	// calculate microseconds to waste during one wall clock slice
	//
	mwaste   = MSLICE * cpuperc / 100;

	// initialize begin wall clock time in microseconds and
	// initialize begin CPU time in microseconds
	//
	clock_gettime(CLOCK_REALTIME, &begwalltime);
	mbegwall = timespec2micro(&begwalltime);

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &begcputime);
	mbegcpu = timespec2micro(&begcputime);

	// main loop
	// ---------------
	// during one wall clock slice:
	// - first consume required percentage of CPU cycles
	// - after that sleep the remaining part of the slice,
	//   unless there is no remaining part when not enough
	//   wasting was possible (not enough CPU capacity)
	//
	for (EVER) {
		// get current wall clock time in microseconds and
		// get current CPU time in microseconds
		//
		clock_gettime(CLOCK_REALTIME, &curwalltime);
		mcurwall = timespec2micro(&curwalltime);

		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &curcputime);
		mcurcpu = timespec2micro(&curcputime);

		// calculate elapsed  wall clock time in microseconds and
		// calculate consumed CPU time in microseconds
		//
		mdifwall = mcurwall - mbegwall;
		mdifcpu  = mcurcpu  - mbegcpu;

		// verify if the wasted CPU time for this slice has been reached
		//
		if (mdifcpu >= mwaste) {
			if (mdifwall < MSLICE) {
				// sleep the rest of this slice
				//
				sleeptime.tv_sec  = 0;
				sleeptime.tv_nsec = (MSLICE - mdifwall) * 1000;
				nanosleep(&sleeptime, (struct timespec *)0);
			}

			// preserve new begin wall clock time in microseconds and
			// preserve new begin CPU time in microseconds
			//
			clock_gettime(CLOCK_REALTIME, &begwalltime);
			mbegwall = timespec2micro(&begwalltime);

                        mbegcpu  = mcurcpu;

			continue;
		}

		// verify if the wall clock slice has passed without consuming
		// the planned CPU time
		//
		if (mdifwall >= MSLICE) { 
			// preserve new begin wall clock time in microseconds and
			// preserve new begin CPU time in microseconds
			//
			clock_gettime(CLOCK_REALTIME, &begwalltime);
			mbegwall = timespec2micro(&begwalltime);

                        mbegcpu  = mcurcpu;
		}
	}

	return NULL;
}


// signal catcher when CPU limit has been reached
//
static void
normalstop(int sig)
{
	exit(0);
}
