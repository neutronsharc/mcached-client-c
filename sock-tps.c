
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

#include "utilities.h"


#ifdef MPI
#include "mpi.h"
#endif


static void help();

// Each client works on this many objs.
static long perClientObjs = 1000;

// Write ratio.
static double writeMixRatio = -1;

// If we should create objs upfront.
static int createObjects = 0;

// string of servers, "srv:port,srv2:port,...".
static char *opt_servers= NULL;

static float updateratio = -1;

// how many keys to get in one get() call.
static int numKeysInOneGet = 1;

#define MAX_KEYS_IN_ONE_GET (256)

typedef struct KVPair KVPair;
struct KVPair{
    char *key;
    int key_length;
    char *value;
    size_t value_length;
};


// Connect to server host at PORT.
static int client_connect(const char* server_name) {
    int PORT = 11211;
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


static unsigned long get_rand(unsigned long max_val) {
  long v = lrand48();
  return v % max_val;
}


/*
 * memcached-set with retry.
 * return 0 on success,
 * -1 if failed even after retries.
 */
int memcache_set_with_retry(memcached_st *memc, KVPair *kvpair, int retry) {
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

// Multi-get.
// Return:  number of hits.
int memcached_multi_get(memcached_st *memc,
                        char **keys,
                        size_t keysLength[],
                        int numKeys) {
  char return_key[MEMCACHED_MAX_KEY];
  size_t return_key_length;
  char *return_value;
  size_t return_value_length;
  memcached_return_t rc;
  rc = memcached_mget(memc, (const char**)keys, keysLength, numKeys);

  if (rc != MEMCACHED_SUCCESS) {
    printf("multi-get ret %s\n", memcached_strerror(memc, rc));
    return 0;
  }

  int getObjs = 0;
  int flags;
  while ((return_value = memcached_fetch(memc,
                                         return_key,
                                         &return_key_length,
                                         &return_value_length,
                                         &flags,
                                         &rc))) {
    getObjs++;
    int pid;
    long vInKey, vInValue;
    return_key[return_key_length] = 0;
    sscanf(return_key, "p%d-key-%ld", &pid, &vInKey);
    sscanf(return_value, "value-of-%ld", &vInValue);
    if (vInValue != vInKey) {
      printf("mget error: key = %s, value = %s\n", return_key, return_value);
    }
    free(return_value);
  }
  return getObjs;
}


// Multi-process transaction throughput test.
int tps_test(memcached_st *memc, int numprocs, int myid) {
  int sizes[6] = {1020, 2020, 3010};  // size should minus 2 (exclude "\r\n")
  int num_sizes = 1;  // We will use 1 size as obj size from the above array:  1020

  long total_numitems = perClientObjs * numprocs;
  long total_ops = perClientObjs * numprocs;

  updateratio = 0;  // as a temp test.
  float ratio_start = 0.0;
  float ratio_end = 1.01;

  // Each proc creates this many objs, then access this many objs.
  long perproc_items= total_numitems / numprocs;
  long myops = total_ops / numprocs;

  long write_failure = 0;
  long read_failure = 0;
  int *write_lats = malloc(myops * sizeof(int)); // record latency in us for each overwrite
  int *rd_lats = malloc(myops * sizeof(int)); // record latency in us for each read
  assert(write_lats && rd_lats);

  long i, j;
  struct timeval t1,t2;
  memcached_return_t rc;
  double tus;
  size_t value_length = 0;
  uint32_t flags;
  int bufsize = 1024 * 1024; //The upper limit of value data size is 1M.
  KVPair pairs;

  if (writeMixRatio >=0) {
    ratio_start = writeMixRatio;
    ratio_end = writeMixRatio;
  }
  if(myid == 0) {
    fprintf(stderr, "\n\n***********\nTotal objects %ld, total op %ld, each "
            "client creates %ld objs, then r/w %ld objs, write-ratio=(%f ~ %f)\n",
            total_numitems, total_ops,  perproc_items, myops, ratio_start, ratio_end);
    for(i = 0; i < num_sizes; i++) {
      fprintf(stderr, "\teach obj size[%ld]=%d\n", i, sizes[i]);
    }
  }

  ////////////////////////////////////////////////////////////////
  /// init the random-gen
  gettimeofday(&t1, NULL);
  srand48(t1.tv_usec);

  pairs.key = (char *)malloc(MEMCACHED_MAX_KEY);
  pairs.value = (char *)malloc(bufsize);
  memset(pairs.value, 'a', bufsize );

  char *mgetKeys[MAX_KEYS_IN_ONE_GET];
  size_t keysLength[MAX_KEYS_IN_ONE_GET];
  for (i = 0; i < MAX_KEYS_IN_ONE_GET; i++) {
    mgetKeys[i] = (char*)malloc(MEMCACHED_MAX_KEY);
  }

  ///////////////////////////////////////////////
  //////////////  0.  create the base data set
  if (createObjects) {
    if(myid == 0) {
      fprintf(stderr, "[p_%d]: Each proc will create %ld objs upfront, obj-size=%d\n",
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
      rc = memcache_set_with_retry(memc, &pairs, 0);
      if( rc != 0 ) {
        fprintf(stderr, "Error::  set, key=%s: val-len=%ld, ret = %d\n",
                pairs.key, pairs.value_length, rc );
        write_failure++;
      }
      if((i + 1) % 500000 == 0) {
        printf("[p_%d]: set %ld items\n", myid, i + 1);
      }
    }
#ifdef MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
    gettimeofday(&t2, NULL);
    tus = timedif_us(t2, t1);
    if (myid == 0) {
      fprintf(stderr, "[p_%d]: Each process has created %ld objs, total %ld objs, "
              "total-time = %.3f sec\n",
              myid, perproc_items, total_numitems, tus / 1000000.0);
      fprintf(stderr, "total set obj size = %.3f MB, tps = %.3f op/sec\n\n\n",
              total_numitems * sizes[0] / 1024.0 / 1024,
              total_numitems / (tus / 1000000.0));
    }
  }

    ///////////////////////////////////////////////
    /////////////   1. transaction test ( set or get )
    struct timeval tstart, tend;
    int opselect = 0;
    int thresh;
    long opset = 0, opget = 0, m1 = 0;
    long tmp;
    long max_write_lat = -1, max_rd_lat = -1;
    long get_miss = 0;

    for(updateratio = ratio_start; updateratio < ratio_end; updateratio += 0.1) {
      thresh = (int)((updateratio * 1000000));
      memset(write_lats, 0, sizeof(int) * myops);
      memset(rd_lats, 0, sizeof(int) * myops);

      if(myid == 0) {
        fprintf(stderr, "\n***** Each process will run %ld cmds, write-ratio %d %%\n",
                myops, (int)(updateratio * 100));
      }
      opset = opget = get_miss = 0;
      max_write_lat = -1;
      max_rd_lat = -1;

      write_failure = 0;
      read_failure = 0;
#ifdef MPI
      MPI_Barrier(MPI_COMM_WORLD);
#endif
      gettimeofday(&t1, NULL);
      for(j = 0; j < myops; j++) {
        // select operation type: the "opselect" is in [0, 1000)
        opselect = get_rand(1000000);
        if (opselect < thresh) {
          // do a write.
          i = get_rand(perproc_items);
          sprintf(pairs.key, "p%d-key-%ld", myid, i);
          pairs.key_length = strlen(pairs.key);
          memset(pairs.value, 0, 1020);
          sprintf(pairs.value, "value-of-%ld", i);
          pairs.value_length = sizes[i % num_sizes];

          flags = i + 1;

          gettimeofday(&tstart, NULL);
          rc = memcache_set_with_retry(memc, &pairs, 0);
          gettimeofday(&tend, NULL);

          if(rc != MEMCACHED_SUCCESS) {
            printf("[p_%d]: set failure, val-len=%ld, ret=%d\n", myid, pairs.value_length, rc);
            write_failure++;
          }
          tmp = timedif_us(tend, tstart);
          write_lats[opset] = (int)tmp;
          max_write_lat = (max_write_lat > tmp) ? max_write_lat : tmp;
          opset++;

        } else {// get-op
          if (numKeysInOneGet > 1) {
            int k;
            for (k = 0; k < numKeysInOneGet; k++) {
              i = get_rand(perproc_items);
              sprintf(mgetKeys[k], "p%d-key-%ld", myid, i);
              keysLength[k] = strlen(mgetKeys[k]);
            }
            gettimeofday(&tstart, NULL);
            int hits = memcached_multi_get(memc, mgetKeys, keysLength, numKeysInOneGet);
            gettimeofday(&tend, NULL);
            if (hits < numKeysInOneGet) {
              get_miss += (numKeysInOneGet - hits);
            }
          } else {
            i = get_rand(perproc_items);
            sprintf(pairs.key, "p%d-key-%ld", myid, i);
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
            if (tmpvalue != NULL) {
              free(tmpvalue);
            }
          }
          tmp = timedif_us(tend, tstart); // tmp in micro-sec
          rd_lats[opget] = (int)tmp;
          max_rd_lat = (max_rd_lat > tmp) ? max_rd_lat : tmp;
          opget++;
        }

        if ((j + 1) % 500000 == 0) {
          printf("[p_%d]: has run %ld ops\n", myid, j + 1);
        }
      }

      gettimeofday(&t2, NULL);
      tus = timedif_us(t2, t1);
      double tps = myops / (tus / 1000000.0);
      double alltps = 0;
      long allset, allget, allmiss;
      long all_read_failure = 0;
      long all_write_failure = 0;
#ifdef MPI
      MPI_Reduce(&tps, &alltps, 1, MPI_DOUBLE_PRECISION, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&opset, &allset, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&opget, &allget, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&get_miss, &allmiss, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&read_failure, &all_read_failure, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
      MPI_Reduce(&write_failure, &all_write_failure, 1, MPI_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
#endif
      if(myid == 0) {
#ifdef MPI
      fprintf(stderr, "-------- In total:  write ratio %d %%\n"
              "%ld get(%d objs in one get), %ld write, %ld get-miss, total-tps= %ld op/s, "
              "%ld read failure, %ld write failure\n",
              (int)(updateratio * 100),
              allget, numKeysInOneGet, allset, allmiss, (long)(alltps),
              all_read_failure, all_write_failure);
#endif
      fprintf(stderr, "[p_%d]: %ld ops, %ld overwrite, %ld get, %ld get-miss, "
              "%ld read failure, %ld write failure: \n"
              "total-time= %f sec, tps = %.3f op/s\n",
              myid, myops, opset, opget, get_miss,
              read_failure, write_failure,
              tus / 1000000.0, tps);
    }

    // sort rd_lats[] and write_lats[], get: 50%, 90%, 95%, 99%, 99.9% latencies
    int rd_lat50, rd_lat90, rd_lat95, rd_lat99, rd_lat999;
    int owrt_lat50, owrt_lat90, owrt_lat95, owrt_lat99, owrt_lat999;
    if (updateratio < 0.999) {
      sort_ascend_int(rd_lats, 0, opget - 1);
      rd_lat50 = rd_lats[(int)(opget * 0.5)];
      rd_lat90 = rd_lats[(int)(opget * 0.9)];
      rd_lat95 = rd_lats[(int)(opget * 0.95)];
      rd_lat99 = rd_lats[(int)(opget * 0.99)];
      rd_lat999 = rd_lats[(int)(opget * 0.999)];
      fprintf(stderr, "[p-%d]: read lat (ms): 50%% = %.3f, 90%% = "
              "%.3f, 95%% = %.3f, "
              "99%% = %.3f, 99.9%% = %.3f, maxlat= %.3f\n",
              myid, rd_lat50/1000.0, rd_lat90/1000.0, rd_lat95/1000.0,
              rd_lat99/1000.0, rd_lat999/1000.0, max_rd_lat/1000.0);
    }
#ifdef MPI
    MPI_Barrier(MPI_COMM_WORLD);
#endif
    if (updateratio > 0.0001) {
      sort_ascend_int(write_lats, 0, opset - 1);
      owrt_lat50 = write_lats[(int)(opset * 0.5)];
      owrt_lat90 = write_lats[(int)(opset * 0.9)];
      owrt_lat95 = write_lats[(int)(opset * 0.95)];
      owrt_lat99 = write_lats[(int)(opset * 0.99)];
      owrt_lat999 = write_lats[(int)(opset * 0.999)];
      fprintf(stderr, "[p-%d]: write_lat (ms): 50%% = %.3f, 90%% = %.3f, 95%% = %.3f, "
              "99%% = %.3f, 99.9%% = %.3f, maxlat= %.3f\n",
              myid, owrt_lat50/1000.0, owrt_lat90/1000.0, owrt_lat95/1000.0,
              owrt_lat99/1000.0, owrt_lat999/1000.0, max_write_lat/1000.0);
      }

      sleep(3); // wait for GC to complete one round of background

   }

  for (i = 0; i < MAX_KEYS_IN_ONE_GET; i++) {
    free(mgetKeys[i]);
  }
   free(pairs.key);
   free(pairs.value);
   free(write_lats);
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

    KVPair kvpair;
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


int main(int argc, char *argv[]) {
  if (argc == 1) {
    help();
    return 1;
  }

  int c;
  while((c = getopt(argc, argv, "s:n:m:k:wh")) != EOF) {
    switch(c) {
      case 's':
        opt_servers = strdup(optarg);
        printf("servers = %s\n", opt_servers);
        break;
      case 'n':
        perClientObjs = atol(optarg);
        printf("each client works on %ld objs\n", perClientObjs);
        break;
      case 'm':
        writeMixRatio = atof(optarg);
        printf("write mix ratio = %f\n", writeMixRatio);
        break;
      case 'w':
        createObjects = 1;
        printf("will create objects upfront.\n");
        break;
      case 'k':
        numKeysInOneGet = atoi(optarg);
        printf("fetch %d objs in one get()\n", numKeysInOneGet);
        assert(numKeysInOneGet <= MAX_KEYS_IN_ONE_GET);
        break;
      case 'h':
        help();
        return 0;
      case '?':
        printf("Unknown options: %s\n", optarg);
      default:
        help();
        return 1;
    }
  }
  if (optind < argc || !opt_servers) {
    help();
    exit(1);
  }


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

  memcached_st *memc;
  memcached_server_st *servers;
  memcached_return_t  rc;

  memc = memcached_create(NULL);
	// the server string is:  "srvhost:port,srvhost:port,srvhost:port".
	// Or,
	//   memcached_server_st * memcached_server_list_append(
  //          memcached_server_st* existing,
  //          char* hostname, int port,
  //          memcached_return_t *rc);
  servers= memcached_servers_parse(opt_servers);
  // add list of servers to the mc-struct
  memcached_server_push(memc, servers);
  int numServers = memcached_server_count(memc);
  printf("client %d will talk with %d servers...\n", myid, numServers);
  memcached_server_list_free(servers);

  rc = memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_TCP_NODELAY, 1);
  assert(rc == MEMCACHED_SUCCESS);

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
  return 0;
}

static void help()
{
  printf("Benchmark Memcached servers performance\n"
         "-s <s1:p1,s2:p2,...> : a list of servers\n"
         "-n <num>             : each client works on this many objects. "
         "                       Each obj is 1KB size.\n"
         "-w                   : Create/write objects upfront.\n"
         "-m <0.x>             : write mix ratio of the benchmark. 0 is read only,\n"
         "                       0.1 is 10%% write, 1 is 100%% write.\n"
         "                       Giving a negative value will cause clients to\n"
         "                       repeat benchmark varying write ratio from 0\n"
         "                       to 1 at 0.1 step.\n"
         "-k <mget>            : number of keys in one get()\n"
         "-h                   : this message.\n");
}
