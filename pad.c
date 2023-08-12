/*
** pad.c -- Process Address space Dump
**
** Peek in the address space of a running process.
**
** Usage:  pad  pid  [hexaddress  [numbytes]]
** ==================================================================
** Author:  Gerlof Langeveld        (2018)
** Copyright (C) 2018  AT Computing BV
** ==================================================================
** This file is free software.  You can redistribute it and/or modify
** it under the terms of the GNU General Public License (GPL); either
** version 3, or (at your option) any later version.
*/

#define	__FILE_OFFSET_BITS	64
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/ptrace.h>
#include <sys/wait.h>


#define	BYTESPERLINE	16
#define	MAXAR		1024

struct arange {
	void		*start;
	long long	length;
	char		name[128];
	char		perm[16];
} ar[MAXAR];

char *usage = "Usage: pad  pid  [hexaddress  [numbytes]]\n";
char dumpall = 1;

static void	dumparea(int, long long, long long, int);
static void	dumpline(long long, unsigned char *, int);
static void	detachproc(int);
static int	getaddranges(long, struct arange [], int);

int
main(int argc, char *argv[])
{
	char 		fname[1000], line[128], *p;
	unsigned char 	*buf;
	int		memfd;
	long long	pid, address, length;
	int		i, n;

	// argument verification
	//
	if (argc < 2 || argc > 4) {
		fprintf(stderr, usage);
		exit(1);
	}

	// argument conversion
	//
        pid = strtoll(argv[1], &p, 10);

	if (*p) {
		fprintf(stderr, usage);
		fprintf(stderr, "invalid pid value\n");
		exit(1);
	}

	if (argc > 2) {
		dumpall = 0;
        	address = strtoll(argv[2], &p, 16);

		if (*p) {
			fprintf(stderr, usage);
			fprintf(stderr, "invalid address value\n");
			exit(1);
		}

		if (argc == 4) {
        		length = strtoll(argv[3], &p, 10);
	
			if (*p) {
				fprintf(stderr, usage);
				fprintf(stderr, "invalid length value\n");
				exit(1);
			}
		} else {
			length = 16;
		}
	}

	// attach target process: required to be able to read memory
	//
        if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) {
            perror("Attach to specified pid");
            exit(1);
        }

        (void) wait(NULL);

	// open memory of target process
	//
        snprintf(fname, sizeof fname, "/proc/%d/mem", pid);

        if ( (memfd = open(fname, O_RDONLY)) == -1) {
        	perror("Open memory of process");
		detachproc(pid);
		exit(1);
        }

	if (dumpall) {
		int nar;

		// determine all address ranges of this process
		//
		if ( (nar = getaddranges(pid, ar, MAXAR)) == -1) {
	            	exit(1);
        	}

		// dump address ranges one-by-one
		//
		for (i=0; i < nar; i++) {
			printf("------------  perms=%s  vsize=%lldKiB  %s\n",
				ar[i].perm, ar[i].length/1024, ar[i].name);

			dumparea(memfd, pid,
				(long long)ar[i].start, ar[i].length);

			printf("\n");
		}
	} else {
		dumparea(memfd, pid, address, length);
	}

	(void) close(memfd);

	// detach process
	//
	detachproc(pid);
}


/*
** read memory area and dump as a number of lines
*/
static void
dumparea(int memfd, long long pid, long long addr, int len)
{
	long		i;
	unsigned char	*buf;

	// seek to requested address and read bunch of bytes
	//
	if ( (buf = malloc(len)) == 0) {
		perror("Can't allocate read buffer");
		detachproc(pid);
		exit(1);
       	}

	if ( lseek(memfd, addr, SEEK_SET) == -1) {
 		perror("Seek to memory address");
		detachproc(pid);
		exit(1);
       	}

	if ( (len = read(memfd, buf, len)) == -1) {
		perror("Read memory address");
		detachproc(pid);
		exit(1);
       	}

	// print hex output line-by-line
	//
	for (i=0; i < len; i+=BYTESPERLINE, addr+=BYTESPERLINE) {
		dumpline(addr, &buf[i],
			len-i>BYTESPERLINE ? BYTESPERLINE:len-i);
	}

	free(buf);
}


/*
** print one line of hexadecimal and character output
*/
static void
dumpline(long long addr, unsigned char *buf, int len)
{
	char	hexpart[BYTESPERLINE*3+1], chars[BYTESPERLINE+1];
	int	i;

	// print address at beginning of line
	//
	printf("%012llx  ", addr);

	// format every byte in hexadecimal and character representation
	//
	for (i=0; i < len; i++) {
		sprintf(hexpart+i*3, "%02x ", buf[i]);

		if (isprint(buf[i]))
			chars[i] = buf[i];	// printable
		else
			chars[i] = '.';		// non-printable
	}

	chars[i] = '\0';

	// print hexadecimal and character representation
	//
	printf("%-*s  %s\n",  BYTESPERLINE*3, hexpart, chars);
}

/*
** detach target process from current process
*/
static void
detachproc(int pid)
{
        (void) ptrace(PTRACE_CONT,   pid, NULL, NULL);
        (void) ptrace(PTRACE_DETACH, pid, NULL, NULL);
}


/*
** get start address and length of every virtual memory area
** in process' address space
*/
static int
getaddranges(long pid, struct arange ar[], int maxar)
{
	FILE	*fp;
   	char	path[128], line[256];
	int	i = 0;
	void	*end;

	snprintf(path, sizeof path, "/proc/%d/smaps", pid);

	if ( (fp = fopen(path, "r")) == NULL) {
		perror("Open smaps");
		return -1;
	}

	while ( fgets(line, sizeof line, fp) )
	{
		if (i >= maxar)
			break;

		if ( sscanf(line, "%p-%p", &(ar[i].start), &end) < 2)
			continue;

		ar[i].name[0] = 0;
		sscanf(line, "%*s %15s %*s %*s %*s %127s",
					ar[i].perm, ar[i].name);

		ar[i].length = end - ar[i].start;

		i++;
	} 

	fclose(fp);

	return i;
}
