/* countcat.C
**
** Measure disk throughput with an artificial load.
**
** Usage: countcat [flags] [filename]
**
** Flags:
**
** --offset number     or  -o number     start reading at offset
** --offsetperc n      or  -%% n         start reading at offset percentage
** --quit quitsize     or  -q quitsize   quit after reading quitsize bytes
** --quittime T        or  -t T          quit after reading T seconds
** --size number       or  -s number     set size (only for ETA computation)
** --bufsize number    or  -b number     set read/write size [128k]
** --random            or  -r            read file at random offsets
** --randomseed        or  -R            seed randomizer, implies -r
** --null              or  -n            don't write (read only)
** --direct            or  -d            O_DIRECT (no caching)
** --directout         or  -D            O_DIRECT (no caching) on stdout
** --interval number   or  -i number     set reporting interval [1]
**
** Numbers for offset, filesize, bufsize may end in K/M/G/T/E/P
** ==========================================================================
** Author:       JC van Winkel
** Copyright (C) 2008  AT Computing
** Modified:     2017  AT Computing -- enlarge counters (Gerlof Langeveld)
** ==========================================================================
** This file is free software.  You can redistribute it and/or modify
** it under the terms of the GNU General Public License (GPL); either
** version 3, or (at your option) any later version.
*/

#define _LARGEFILE64_SOURCE 1
#define _GNU_SOURCE 1

#include <getopt.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <time.h>
#include <errno.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <algorithm>
#include <vector>
using namespace std;

/*
 * 1.1: Wed Oct  1 07:38:58 CEST 2008
 * added -R flag for random seed
 */

static char version[]="1.2";
 
static clock_t starttime;
static long long lasttotcount=0,totcount=0;
static long long filesize=0;
static long long quitsize=0;
static long long offset=0;
static long quittime=0;
static double offsetperc=0.0;
static bool randomize=false;
static int interval=1;
static long lastprinttime;
static vector<long> blocklist;
	        
double getelapstime() {
  struct tms buf;
  clock_t now= times(&buf);

  return (now-starttime)*1.0/ sysconf(_SC_CLK_TCK);
}


void printtime(long t) {
  int s=t%60;
  t /= 60;
  int m=t%60;
  t /= 60;
  if (t) {
    fprintf(stderr, "%2ld:%02d:%02d", t, m, s);
  } else {
    fprintf(stderr, "%2d:%02d", m, s);
  }
    
}


void printnum(double num) {
  int index=0;
  const char *scale=" KMGTPEZ";

  while (num>=1024) {
    num/=1024;
    index++;
  }
  fprintf(stderr, "%7.2f%c%cB",  num, scale[index], index ? 'i':' ' );
}


void printall(int forceprint) {

  if (forceprint || time(0) >= lastprinttime+interval) {

    double elaps=getelapstime();

    printnum(totcount+offset);
    if (filesize || quitsize) {
      double theend;
      if (quitsize) {
        theend=(quitsize+offset) < filesize ? quitsize+offset : filesize;
      } else theend=filesize;
      double done=(double)(totcount)/(theend-offset);
      double rest=(1-done)/done*elaps;
      fprintf(stderr, " %5.1f%% T:", 100.0*done);
      printtime(elaps);
      fprintf(stderr, " ETA:");
      printtime(rest);
    }

    double speed=(totcount)/(elaps+0.00001);
    fprintf(stderr, " Speed:");
    printnum(speed);
    time_t deltat=(time(0)-lastprinttime);
    fprintf(stderr,"/s");
    if (forceprint && time(0) == lastprinttime) {
      fprintf(stderr, "\n");
      return;
    }
    fprintf(stderr,", %3lds:", time(0)-lastprinttime);
    speed=(totcount-lasttotcount)/(time(0)-lastprinttime+0.00001);
    printnum(speed);
    fprintf(stderr,"/s\n");
    lasttotcount=totcount;
    lastprinttime=time(0);

  }
}

long long getnum(const char *s) {
  long long thenum;
  char *endptr;

  thenum=strtoll(s, &endptr, 10);
  switch (*endptr) {
  case 'K':
  case 'k':
      thenum*=1024LL;
      break;
  case 'M':
  case 'm':
      thenum*=1024LL*1024;
      break;
  case 'G':
  case 'g':
      thenum*=1024LL*1024*1024;
      break;
  case 'T':
  case 't':
      thenum*=1024LL*1024*1024*1024;
      break;
  case 'P':
  case 'p':
      thenum*=1024LL*1024*1024*1024*1024;
      break;
  case 'E':
  case 'e':
      thenum*=1024LL*1024*1024*1024*1024*1024;
      break;
  }
  return thenum;
}

int main(int argc, char *argv[]) {
  char *buf;
  int n,m;
  int fd=0;
  int i;
  int nullout=0;
  int direct=0;
  int directout=0;
  int randseed=0;
  long bufsize=128*1024;
  const char *filename=0;

  char *endptr;

  lastprinttime=time(0);
  struct stat64 statbuf;
  static struct option long_options[] = {
      {"offset", 1, 0, 'o'},
      {"offsetperc", 1, 0, '%'},
      {"quit", 1, 0, 'q'},
      {"timequit", 1, 0, 't'},
      {"size", 1, 0, 's'},
      {"bufsize", 1, 0, 'b'},
      {"random", 0, 0, 'r'},
      {"randomseed", 0, 0, 'R'},
      {"null", 0, 0, 'n'},
      {"direct", 0, 0, 'd'},
      {"directout", 0, 0, 'D'},
      {"version", 0, 0, 'V'},
      {0, 0, 0, 0}
  };

  while (1) {
    int option_index=0;
    int c=getopt_long(argc, argv, "VdDnrb:%:R:t:s:q:o:", long_options, &option_index);
    if (c==-1) {
      break;
    }
    
    switch (c) {
    case '%':
      offsetperc=getnum(optarg);
      break;
    case 'o':
      offset=getnum(optarg);
      break;
    case 't':
      quittime=getnum(optarg);
      break;
    case 'q':
      quitsize=getnum(optarg);
      break;
    case 's':
      filesize=getnum(optarg);
      break;
    case 'b':
      bufsize=getnum(optarg);
      break;
    case 'i':
      interval=atoi(optarg);
      break;
    case 'R':
      randseed=atoi(optarg);
      srand(randseed);
      // NO BREAK
    case 'r':
      randomize=true;
      break;
    case 'n':
      nullout=1;
      break;
    case 'V':
      fprintf(stderr, "%s version %s\n", argv[0], version);
      exit(0);
      break;
    case 'D':
      directout=O_DIRECT;
      break;
    case 'd':
      direct=O_DIRECT;
      break;
    default:
      fprintf(stderr, "Usage: %s [options] [filename]\n"
      "Options:\n"
      "--offset number   or -o number    start reading at offset\n"
      "--offsetperc n    or -%% n        start reading at offset percentage\n"
      "--quit quitsize   or -q quitsize  quit after reading quitsize bytes\n"
      "--quittime T      or -t T         quit after reading T seconds\n"
      "--size number     or -s number    set size (only for ETA computation)\n"
      "--bufsize number  or -b number    set read/write size [128k]\n"
      "--random          or -r           read file at random offsets\n"
      "--randomseed      or -R           seed randomizer, implies -r\n"
      "--null            or -n           don't write (read only)\n"
      "--direct          or -d           O_DIRECT (no caching)\n"
      "--directout       or -D           O_DIRECT (no caching) on stdout\n"
      "--interval number or -i number    set reporting interval [1]\n"
      "", argv[0]);
      fprintf(stderr, "Numbers for offset, filesize, bufsize may end in K/M/G/T/E/P\n");
      exit(1);
    } 
  } 
  if (optind < argc) {
    filename=argv[optind];
  }

  buf=(char *)malloc(bufsize+512); // +512 for allignment on page boundary
  if (!buf) {
    fprintf(stderr, "cannot allocate %ld bytes for buffer\n", bufsize);
    exit(1);
  }


  // allign buffer on 512-byte boundary
  while ((unsigned long)buf & 0x1ff)
    buf++;

  if (filename && (fd=open(filename, O_RDONLY|O_LARGEFILE|direct))<0) {
    fprintf(stderr, "cannot open: %s: ", filename);
    perror("");
    exit(1);
  }

  // get file size if possible
  if (filesize==0 && fstat64(fd, &statbuf)>=0) {
    if (S_ISREG(statbuf.st_mode)) {
      filesize=statbuf.st_size;
    } else if (S_ISBLK(statbuf.st_mode)) {
      // linux specific!
      long blksize;	// modified 2017-11-08 Gerlof Langeveld
      int  blks;
      if (ioctl(fd, BLKGETSIZE, &blksize) == 0 &&
          ioctl(fd, BLKSSZGET, &blks)==0) {
        filesize=(long long)blksize * blks;
      }
    }
  }

  if (filesize && offsetperc) {
    offset=filesize*offsetperc/100.0;
    offset &= (~511);
  }

  if (randomize) {
    long nblocks=filesize/bufsize;  
    for (int i=0; i<nblocks; ++i) {
      blocklist.push_back(i);
    }
    random_shuffle(blocklist.begin(), blocklist.end());
  }

  if (directout && fcntl(1, F_SETFL, directout)<0) {
      fprintf(stderr, "cannot set O_DIRECT flag on stdout: ");
      perror("");
      exit(1);
  }

  if (fd && direct && fcntl(fd, F_SETFL, direct)<0) {
      fprintf(stderr, "cannot set O_DIRECT flag: ");
      perror("");
      exit(1);
  }

  if (offset) {
    if (lseek64(fd, offset, 0)<0) {
      fprintf(stderr, "cannot seek to position %lld: ", offset);
      perror("");
      exit(1);
    }
  }
    

  {
    struct tms dummybuf;
    starttime=times(&dummybuf);
  }

  bool end=false;
  while (!end) {
    if (randomize) {
      if (blocklist.size()==0) {
        n=0;
        break;
      }
      long long blockno=blocklist.back();
      blocklist.pop_back();
      long long offset=blockno*bufsize;
      if (lseek64(fd, offset, 0) != offset) {
        n=-1;
        break;
      }
    }
    n=read(fd, buf, bufsize);

    if (n<=0) break;

    if (nullout) {
      m=n;
    } else {
      m=write(1, buf, n);
    }
    totcount+=m;
    if (m<=0) break;
    printall(0);
    if (quitsize && totcount >= quitsize) break;
    if (quittime && getelapstime() >= quittime) break;
  }
  
  if (totcount>0) {
     printall(1);
  } else if (n<0) {
    fprintf(stderr, "error reading from file: ");
    perror("");
    exit(1);
  }

  exit(m<=0);
}
