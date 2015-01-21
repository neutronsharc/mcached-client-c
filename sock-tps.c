/* LibMemcached
 * Copyright (C) 2006-2009 Brian Aker
 * All rights reserved.
 *
 * Use and distribution licensed under the BSD license.  See
 * the COPYING file in the parent directory for full text.
 *
 * Summary:
 *
 */

//#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>

#include <libmemcached/memcached.h>
#include "client_options.h"
#include "utilities.h"


#ifdef MPI
#include "mpi.h"
#endif

#define PROGRAM_NAME "sock-tps"
#define PROGRAM_DESCRIPTION "measure tps to memcached cluster."

#define PORT 11211

/* Prototypes */
static void options_parse(int argc, char *argv[]);
static void usage(void);

static int opt_binary=0;
static int opt_verbose= 0;
static char *opt_servers= NULL;
static int opt_server_port = 11350;
static char *opt_host= NULL;
static char *opt_hash= NULL;
static int opt_method= OPT_SET;
static uint32_t opt_flags= 0;
static time_t opt_expires= 0;
static char *opt_username;
static char *opt_passwd;
static char *recv_buffer;

static float updateratio = -1;

typedef struct pairs_st pairs_st;
struct pairs_st{
    char *key;
    int key_length;
    char *value;
    size_t value_length;
};

static long strtol_wrapper(const char *nptr, int base, bool *error) {
  long val;
  char *endptr;

  errno= 0;    /* To distinguish success/failure after call */
  val= strtol(nptr, &endptr, base);

  /* Check for various possible errors */
  if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
      || (errno != 0 && val == 0)) {
    *error= true;
    return 0;
  }

  if (endptr == nptr) {
    *error= true;
    return 0;
  }

  *error= false;
  return val;
}


/* Connect to server host at PORT. */
static int client_connect( const char* server_name ) {
    struct addrinfo *res, *t;
    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    char *service;
    int n;
    int sockfd = -1;

    if (asprintf(&service, "%d", PORT) < 0) {
        return -1;
    }

    n = getaddrinfo(server_name, service, &hints, &res);
    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), server_name, PORT);
        return n;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }
    freeaddrinfo(res);
    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s:%d\n", server_name, PORT);
        return sockfd;
    }
    return sockfd;
}


unsigned long get_rand(unsigned long max_val)
{
    long v; // = rand();
    v = lrand48();
    return v%max_val;
}


/*
 * memcached-set with retry.
 * return 0 on success,
 * -1 if failed even after retries.
 */
int memcache_set_with_retry(memcached_st *memc, pairs_st *kvpair, int retry) {
  memcached_return_t rc;
  int expireTime = 0;
  int flags = 0;
  if (retry < 0) {
    retry = 0;
  }
  int retried = 0;
  while (retried <= retry) {
    rc = memcached_set(memc,
                       kvpair->key, kvpair->key_length,
                       kvpair->value, kvpair->value_length,
                       expireTime, flags);
    if(rc == MEMCACHED_SUCCESS) {
      return rc;
    } else if (rc == MEMCACHED_IN_PROGRESS) {
      printf("key: %s, retry = %d\n", kvpair->key, retried);
      retried++;
    } else {
      printf("key: %s, set error at retry %d: ret = %d\n",
             kvpair->key, retried, rc);
      break;
    }
  }
  return -1;
}

#if 0
    long perclient_numitems = 1000L * 200;
    long perclient_ops = 1000L * 200;
#else
    long perclient_numitems = 1000L * 1000;
    long perclient_ops = 1000L * 1000;
#endif

/* Multi-process transaction throughput test. Should run with MPI. */
int tps_test(memcached_st *memc, int numprocs, int myid) {
  int sizes[6] = {1020, 2020, 3010};  // size should minus 2 (exclude "\r\n")
  int num_sizes = 1;  // We will use 1 size as obj size from the above array:  1020

  long total_numitems = perclient_numitems * numprocs;
  long total_ops = perclient_ops  * numprocs;

  updateratio = 0;  // as a temp test.
  float ratio_start = 0.0;
  float ratio_end = 1.01;

  long perproc_items= total_numitems / numprocs;  // each procs inserts this many items
  long myops = total_ops / numprocs;   //then, each proc performs this many trans

  int owrt_failure = 0;
  int read_failure = 0;
  int *owrt_lats = malloc(myops * sizeof(int)); // record latency in us for each overwrite
  int *rd_lats = malloc(myops * sizeof(int)); // record latency in us for each read
  assert(owrt_lats && rd_lats);

  long i, j;
  struct timeval t1,t2;
  memcached_return_t rc;
  double tus;
  size_t value_length = 0;
  uint32_t flags;
  int bufsize = 1024 * 1024; //The upper limit of value data size is 1M.
  pairs_st pairs;

  if (perproc_items * numprocs < total_numitems &&  myid == 0) {
    perproc_items = total_numitems - perproc_items * (numprocs - 1);
  }

  if (myops * numprocs < total_ops &&  myid == 0 ) {
    myops = total_ops - myops * (numprocs - 1);
  }

  if(myid == 0) {
    fprintf(stderr, "\n\n***********\nTotal objects %ld, total op %ld, each "
            "client creates %ld items, then %ld ops, update-ratio=(%f ~ %f)\n",
            total_numitems, total_ops,  perproc_items, myops, ratio_start, ratio_end);
    for(i = 0; i < num_sizes; i++) {
      fprintf(stderr, "\teach obj size[%ld]=%d\n", i, sizes[i]);
    }
  }
  //sleep(2);

  ////////////////////////////////////////////////////////////////
  /// init the random-gen
  gettimeofday(&t1, NULL);
  srand48( t1.tv_usec );

  pairs.key = (char *)malloc(128);
  pairs.value = (char *)malloc(bufsize);
  memset(pairs.value, 'a', bufsize );

  ///////////////////////////////////////////////
  //////////////  0.  create the base data set
  if( myid==0 ) {
    fprintf(stderr, "[p_%d]: Each proc will create %ld items as base, item-size=%d\n",
            myid, perproc_items, sizes[0] );
  }

#ifdef MPI
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  gettimeofday(&t1, NULL);
  time_t expireTime = 0;
  for (i = 0; i < perproc_items; i++) {
    sprintf(pairs.key, "p%d-key-%ld", myid, i);
    pairs.key_length = strlen(pairs.key);

    sprintf(pairs.value, "value-of-%ld", i);
    pairs.value_length = sizes[i % num_sizes];
    flags = i;
    /*rc = memcached_set(memc,
                       pairs->key, pairs->key_length,
                       pairs->value, pairs->value_length,
                       expireTime, flags);*/
    rc = memcache_set_with_retry(memc, &pairs, 0);
    if( rc != 0 ) {
      fprintf(stderr, "Error::  set, key=%s: val-len=%ld, ret = %d\n",
              pairs.key, pairs.value_length, rc );
      owrt_failure++;
    }
    if((i + 1) % 100000 == 0) {
      printf("[p_%d]: set %ld items\n", myid, i + 1);
    }
  }
#ifdef MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
    gettimeofday(&t2, NULL);
    tus = (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec);
    if(myid==0) {
      fprintf(stderr, "[p_%d]: Each process has created %ld objs, total %ld objs, "
              "total-time = %.3f sec,  avg = %.3f us / obj\n",
              myid, perproc_items, total_numitems,
              tus / 1000000.0, tus / perproc_items);
      fprintf(stderr, "total set obj size = %.3f MB, tps = %.3f op/sec\n\n\n",
              total_numitems * sizes[0] / 1024.0 / 1024,
              total_numitems / (tus / 1000000.0));
    }

    ///////////////////////////////////////////////
    /////////////   1. transaction test ( set or get )
    struct timeval tstart, tend;
    int opselect = 0;
    int thresh;
    long opset = 0, opget = 0, m1 = 0;
    long tmp;
    long  max_owrt_lat = -1, max_rd_lat = -1;
    long get_miss = 0;

    for(updateratio = ratio_start; updateratio < ratio_end; updateratio += 0.1) {
      thresh = (int)((updateratio * 1000));
      memset(owrt_lats, 0, sizeof(int) * myops);
      memset(rd_lats, 0, sizeof(int) * myops);

      if(myid == 0) {
        fprintf(stderr, "\n***** Each process will run %ld ops, write-ratio %d \%\n",
                myops, (int)(updateratio * 100), thresh );
      }
      opset = opget = get_miss = 0;
      max_owrt_lat = -1;
      max_rd_lat = -1;

      owrt_failure = 0;
      read_failure = 0;
#ifdef MPI
      MPI_Barrier(MPI_COMM_WORLD);
#endif
      gettimeofday(&t1, NULL);
      for(j = 0; j < myops; j++) {
        // select operation type: the "opselect" is in [0, 1000)
        opselect = get_rand(1000);
        if(opselect < thresh) {
          // overwrite the item
          i = get_rand(perproc_items);
          //sprintf(pairs.key, "p%ld-key-%ld", j % numprocs, i);
          sprintf(pairs.key, "p%ld-key-%ld", myid, i);
          pairs.key_length = strlen(pairs.key);
          sprintf(pairs.value, "value-of-%ld", i);
          pairs.value_length = sizes[i % num_sizes];

          flags = i + 1;

          gettimeofday(&tstart, NULL);
          rc = memcache_set_with_retry(memc, &pairs, 0);
          gettimeofday(&tend, NULL);

          if(rc != MEMCACHED_SUCCESS) {
            printf("[p_%d]: set failure, val-len=%ld, ret=%d\n", myid, pairs.value_length, rc);
            owrt_failure++;
          }
          tmp = timedif_us(tend, tstart);
          owrt_lats[opset] = (int)tmp;
          max_owrt_lat = (max_owrt_lat > tmp) ? max_owrt_lat : tmp;
          opset++;

        } else {// get-op
          i = get_rand(perproc_items);
          sprintf(pairs.key, "p%ld-key-%ld", myid, i);
          pairs.key_length = strlen(pairs.key);
          gettimeofday(&tstart, NULL);
          char *tmpvalue = NULL;
          tmpvalue = memcached_get(memc,
                                pairs.key, pairs.key_length,
                                &value_length, &flags, &rc);
          gettimeofday(&tend, NULL);
          if(rc != MEMCACHED_SUCCESS) {
            get_miss++;
          } else {
            sscanf(tmpvalue, "value-of-%ld", &m1);
            if (value_length > sizes[i % num_sizes]) {
              // original version of mc-srv: the len = (real-data-len) + 2 (\r\n)
              value_length -= 2;
            }
            if (m1 != i || value_length != sizes[i % num_sizes]){
              printf("[p_%d]: Error!! item-get(%s:%s): %ld:%ld, should be %ld:%d\n",
                     myid, pairs.key, tmpvalue, m1, value_length, i, sizes[i % num_sizes]);
              read_failure++;
            }
          }
          tmp = timedif_us(tend, tstart); // tmp in micro-sec
          if (tmpvalue != NULL) {
            free(tmpvalue);
          }
          rd_lats[opget] = (int)tmp;
          max_rd_lat = (max_rd_lat > tmp) ? max_rd_lat : tmp;
          opget++;
        }

        if ((j + 1) % 100000 == 0) {
          printf("[p_%d]: has run %ld ops\n", myid, j + 1);
        }
      }

      gettimeofday(&t2, NULL);
      tus = (t2.tv_sec - t1.tv_sec) * 1000000 + (t2.tv_usec - t1.tv_usec);
      double tps = myops / (tus / 1000000.0);
      double alltps = 0;
      long allset, allget, allmiss;
      long all_read_failure = 0;
      long all_owrt_failure = 0;
#ifdef MPI
      MPI_Reduce(&tps, &alltps, 1, MPI_DOUBLE_PRECISION, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&opset, &allset, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&opget, &allget, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&get_miss, &allmiss, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&read_failure, &all_read_failure, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&owrt_failure, &all_owrt_failure, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
#endif
      if(myid == 0) {
#ifdef MPI
      fprintf(stderr, "-------- In total:  write ratio %d \%\n"
              "%ld get, %ld write, %ld get-miss, total-tps= %d op/s, "
              "%ld read failure, %ld write failure\n",
              (int)(updateratio * 100),
              allget, allset, allmiss, (int)(alltps),
              all_read_failure, all_owrt_failure);
#endif
      fprintf(stderr, "[p_%d]: %ld ops, %ld overwrite, %ld get, %ld get-miss, "
              "%ld read failure, %ld write failure: \n"
              "total-time= %f sec, tps = %.3f op/s\n",
              myid, myops, opset, opget, get_miss,
              read_failure, owrt_failure,
              tus / 1000000.0, tps);
    }

    // sort rd_lats[] and owrt_lats[], get: 50%, 90%, 95%, 99%, 99.9% latencies
    int rd_lat50, rd_lat90, rd_lat95, rd_lat99, rd_lat999;
    int owrt_lat50, owrt_lat90, owrt_lat95, owrt_lat99, owrt_lat999;
    if (updateratio < 0.999) {
      sort_ascend_int(rd_lats, 0, opget - 1);
      rd_lat50 = rd_lats[(int)(opget * 0.5)];
      rd_lat90 = rd_lats[(int)(opget * 0.9)];
      rd_lat95 = rd_lats[(int)(opget * 0.95)];
      rd_lat99 = rd_lats[(int)(opget * 0.99)];
      rd_lat999 = rd_lats[(int)(opget * 0.999)];
      fprintf(stderr, "[p-%d]: read lat (ms): 50\% = %.3f, 90\% = "
              "%.3f, 95\% = %.3f, "
              "99\% = %.3f, 99.9\% = %.3f, maxlat= %.3f\n",
              myid, rd_lat50/1000.0, rd_lat90/1000.0, rd_lat95/1000.0,
              rd_lat99/1000.0, rd_lat999/1000.0, max_rd_lat/1000.0);
    }
#ifdef MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
    if (updateratio > 0.0001) {
      sort_ascend_int(owrt_lats, 0, opset - 1);
      owrt_lat50 = owrt_lats[(int)(opset * 0.5)];
      owrt_lat90 = owrt_lats[(int)(opset * 0.9)];
      owrt_lat95 = owrt_lats[(int)(opset * 0.95)];
      owrt_lat99 = owrt_lats[(int)(opset * 0.99)];
      owrt_lat999 = owrt_lats[(int)(opset * 0.999)];
      fprintf(stderr, "[p-%d]: write_lat (ms): 50\% = %.3f, 90\% = %.3f, 95\% = %.3f, "
              "99\% = %.3f, 99.9\% = %.3f, maxlat= %.3f\n",
              myid, owrt_lat50/1000.0, owrt_lat90/1000.0, owrt_lat95/1000.0,
              owrt_lat99/1000.0, owrt_lat999/1000.0, max_owrt_lat/1000.0);
      }

      sleep(3); // wait for GC to complete one round of background

   }

   free(pairs.key);
   free(pairs.value);
   free(owrt_lats);
   free(rd_lats);

   return 0;
}

void simple_set_get(memcached_st *memc) {
    long i, id;
    long rlen;
    long numitems = 4000 * 10;
    memcached_return_t  rc;
    char    key[128];
    char    value[1000];
    int valsize = 1000;
    int flags;
    int k;

    pairs_st kvpair;
    kvpair.key = key;
    kvpair.value = value;
    memset(value, 'A', valsize);
    value[valsize - 1] = 0;

    char *rvalue;
    for (i = 0; i < numitems; i++) {
        sprintf(key, "key-%ld", i);
        kvpair.key_length = strlen(key);

        sprintf(value, "value-of-key-%ld", i);
        k = strlen(value);
        memset(value + k, 'A', valsize - k);
        value[valsize - 1] = 0;
        kvpair.value_length = valsize - 1;

        flags = i;
        //rc = memcached_set(memc, key, strlen(key), value, strlen(value), 0, flags);
        rc = memcache_set_with_retry(memc, &kvpair, 100);
        if( rc != MEMCACHED_SUCCESS ) {
            fprintf(stderr, "Set: key=%s: val-len=%ld, ret=%d\n", key, strlen(value), rc);
            assert(0);
        }

        if ((i + 1) % 5000 == 0) {
            fprintf(stderr, "has set %ld\n", i + 1);
        }
    }

    for (i = 0; i < numitems; i++) {
        sprintf(key, "key-%ld", i);
        rvalue = memcached_get(memc, key, strlen(key), &rlen, &flags, &rc);
        if (rc != MEMCACHED_SUCCESS) {
            fprintf(stderr, "Get: key=%s: ret=%ld, rc=%d\n", key, strlen(value), rc );
            assert(0);
        }

        sscanf(rvalue, "value-of-key-%ld", &id);
        assert(rlen == valsize - 1);
        assert(id == i);
        //assert(flags == i);
        free(rvalue);
        if ((i + 1) % 5000 == 0) {
            fprintf(stderr, "has get %ld\n", i + 1);
        }
    }

}

int main(int argc, char *argv[])
{
    memcached_st *memc;
    memcached_server_st *servers;
    memcached_return_t  rc;

    int sockfd;
    int i;

    int return_code= 0;

    options_parse(argc, argv);

    int numprocs;
    int myid;
#ifdef MPI
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);
#else
	numprocs = 1;
	myid = 0;
#endif

    memc= memcached_create(NULL);
    process_hash_option(memc, opt_hash);

    if (!opt_servers) {
        usage();
        exit(1);
    }

	/*
	 the serv string is:  "srvhost:port,srvhost:port,srvhost:port".
	 Or,
	memcached_server_st * memcached_server_list_append(
		memcached_server_st* existing, char* hostname, int port,
		memcached_return_t *rc);
	*/
    if (opt_servers) {
        servers= memcached_servers_parse(opt_servers);
    }
    else {
        servers= memcached_servers_parse(argv[--argc]);
    }

    // add list of servers to the mc-struct
    i = memcached_server_count(memc);
    memcached_server_push(memc, servers);
    i = memcached_server_count(memc);
    printf("will talk with %d servers...\n", i);

    memcached_server_list_free(servers);
    // change mc behaviors...
    if (opt_binary) {
        rc = memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, 1);
        assert(rc == MEMCACHED_SUCCESS);
    }
    rc = memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_TCP_NODELAY, 1);
    assert(rc == MEMCACHED_SUCCESS);

    int bufsize = 1024*1024; //The upper limit of value data size is 1M.
    int dist;
    if (argc > 4) {
        if (argv[4] != NULL)
            dist = atoi(argv[4]);
        else
            dist = 1;
    }
    ////////////////////////////////////////////////////////////////
    //simple_set_get(memc);
    tps_test(memc, numprocs, myid);
    ////////////////////////////////////////////////////////////////

#ifdef MPI
    MPI_Finalize();
#endif

    memcached_free(memc);

    if (opt_servers)
        free(opt_servers);
    if (opt_host)
        free( opt_host );
    if (opt_hash)
        free(opt_hash);
    return return_code;
}

#define OPT_UP  (0x5001)

static void usage(void)
{
    printf("<client>    server:port           (for single server)\n"
           "<client>    <s1:p1,s2:p2,s3:p3,...>     (Use multiple servers)\n"
           "-j <num>    Each client process creates this many objects upfront.\n"
           "-o <num>    Each client process conducts this many operations each round.\n");
}

static void options_parse(int argc, char *argv[])
{
    int option_index= 0;
    int option_rv;

    memcached_programs_help_st help_options[]=
    {
      {0},
    };

    static struct option long_options[]=
    {
        {(OPTIONSTRING)"version", no_argument, NULL, OPT_VERSION},
        {(OPTIONSTRING)"help", no_argument, NULL, OPT_HELP},
        {(OPTIONSTRING)"verbose", no_argument, &opt_verbose, OPT_VERBOSE},
        {(OPTIONSTRING)"debug", no_argument, &opt_verbose, OPT_DEBUG},
        {(OPTIONSTRING)"flag", required_argument, NULL, OPT_FLAG},
        {(OPTIONSTRING)"expire", required_argument, NULL, OPT_EXPIRE},
        {(OPTIONSTRING)"set",  no_argument, NULL, OPT_SET},
        {(OPTIONSTRING)"add",  no_argument, NULL, OPT_ADD},
        {(OPTIONSTRING)"replace",  no_argument, NULL, OPT_REPLACE},
        {(OPTIONSTRING)"hash", required_argument, NULL, OPT_HASH},
        {(OPTIONSTRING)"binary", no_argument, NULL, OPT_BINARY},
        {(OPTIONSTRING)"username", required_argument, NULL, OPT_USERNAME},
        {(OPTIONSTRING)"password", required_argument, NULL, OPT_PASSWD},
        {(OPTIONSTRING)"host", required_argument, NULL, OPT_HOST},
        {(OPTIONSTRING)"upratio", required_argument, NULL, OPT_UP},
        {(OPTIONSTRING)"port", required_argument, NULL, OPT_SERVER_PORT},
        {(OPTIONSTRING)"servers", required_argument, NULL, OPT_SERVERS},
        {(OPTIONSTRING)"objs", required_argument, NULL, OPT_OBJS},
        {(OPTIONSTRING)"ops", required_argument, NULL, OPT_OPS},
        {0, 0, 0, 0},
    };

  while (1)
  {
    option_rv= getopt_long(argc, argv, "Vhvds:p:j:o:", long_options, &option_index);

    if (option_rv == -1) break;

    switch (option_rv)
    {
    case 0:
      break;

    case OPT_UP:
        sscanf(optarg, "%f", &updateratio);
        //printf("opt-up: %s = %f\n", optarg, updateratio);
        break;
    case OPT_BINARY:
      opt_binary = 1;
      break;
    case OPT_VERBOSE: /* --verbose or -v */
      opt_verbose = OPT_VERBOSE;
      break;
    case OPT_DEBUG: /* --debug or -d */
      opt_verbose = OPT_DEBUG;
      break;
    case OPT_VERSION: /* --version or -V */
      version_command(PROGRAM_NAME);
      break;
    case OPT_HELP: /* --help or -h */
      help_command(PROGRAM_NAME, PROGRAM_DESCRIPTION, long_options, help_options);
      break;
    case OPT_SERVERS: /* --servers or -s */
      fprintf(stderr, "server = %s\n", optarg);
      opt_servers= strdup(optarg);
      break;
    case OPT_SERVER_PORT:  //  --port or -p
      opt_server_port = atoi(optarg);
      break;
    case OPT_OBJS:  //  -j
      fprintf(stderr, "Each client creates %s objs\n", optarg);
      perclient_numitems = atol(optarg);
      break;
    case OPT_OPS:  //  --port or -p
      fprintf(stderr, "Each client runs %s ops\n", optarg);
      perclient_ops = atol(optarg);
      break;
    case OPT_HOST: /* --host */
      opt_host= strdup(optarg);
      break;
    case OPT_FLAG: /* --flag */
      {
        bool strtol_error;
        opt_flags= (uint32_t)strtol_wrapper(optarg, 16, &strtol_error);
        if (strtol_error == true)
        {
          fprintf(stderr, "Bad value passed via --flag\n");
          exit(1);
        }
        break;
      }
    case OPT_EXPIRE: /* --expire */
      {
        bool strtol_error;
        opt_expires= (time_t)strtol_wrapper(optarg, 16, &strtol_error);
        if (strtol_error == true)
        {
          fprintf(stderr, "Bad value passed via --flag\n");
          exit(1);
        }
      }
    case OPT_SET:
      opt_method= OPT_SET;
      break;
    case OPT_REPLACE:
      opt_method= OPT_REPLACE;
      break;
    case OPT_ADD:
      opt_method= OPT_ADD;
      break;
    case OPT_HASH:
      opt_hash= strdup(optarg);
      break;
    case OPT_USERNAME:
      opt_username= optarg;
      break;
    case OPT_PASSWD:
      opt_passwd= optarg;
      break;
   case '?':
      /* getopt_long already printed an error message. */
      exit(1);
    default:
      abort();
    }
  }
}
