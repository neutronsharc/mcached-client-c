#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "utilities.h"


long timedif_us(struct timeval end, struct timeval start)
{
  long us, s;

  us = (int)(end.tv_usec - start.tv_usec);
  s = (int)(end.tv_sec - start.tv_sec);
  return s * 1000000 + us;
}


inline void swap_int(int *a, int *b)
{
    int t = *a;
    *a = *b;
    *b = t;
}

/*
Sort an int array in ascending order. the elems-index to be sorted are
    from [begin, end], inclusive.
NOTE: this is an in-place sort.
Return: num of swaps to sort this array.
*/
unsigned long sort_ascend_int(int *array, int begin, int end)
{
    if (end <= begin) { //only 1 elem, return directly.
        return 0;
    }

    long swpcnt = 0;
    //int mid = (begin >> 1) + (end >> 1);
    int pivot = array[begin];

    int left = begin + 1;  // left boundary, inclusive
    int right = end;        // right boundary, inclusive

    // partition the array around the pivot:
    while (left <= right) {
        // forward the left-range
        while (left <= end && array[left] <= pivot) {
            left++;
        }
        // backward the right-range
        while (right >= begin && array[right] > pivot) {
            right--;
        }
        if (left < right) {
            // now, swap left <=> right
            swap_int(array + left, array + right);
            left++;
            right--;
            swpcnt++;
        }
    }

    // now, array[left-1] is the pivot. ( X <= pivot), pivot, ( pivot < Y )
    left--;
    swap_int(array + left, array + begin);
    swpcnt++;
    swpcnt += sort_ascend_int(array, begin, left - 1);
    swpcnt += sort_ascend_int(array, left + 1, end);
    return swpcnt;
}

void version_command(const char *command_name)
{
  printf("%s v%u.%u\n", command_name, 1U, 0U);
  exit(0);
}

static const char *lookup_help(memcached_options option)
{
  switch (option)
  {
  case OPT_SERVERS: return("List which servers you wish to connect to.");
  case OPT_VERSION: return("Display the version of the application and then exit.");
  case OPT_HELP: return("Display this message and then exit.");
  case OPT_VERBOSE: return("Give more details on the progression of the application.");
  case OPT_DEBUG: return("Provide output only useful for debugging.");
  case OPT_FLAG: return("Provide flag information for storage operation.");
  case OPT_EXPIRE: return("Set the expire option for the object.");
  case OPT_SET: return("Use set command with memcached when storing.");
  case OPT_REPLACE: return("Use replace command with memcached when storing.");
  case OPT_ADD: return("Use add command with memcached when storing.");
  case OPT_SLAP_EXECUTE_NUMBER: return("Number of times to execute the given test.");
  case OPT_SLAP_INITIAL_LOAD: return("Number of key pairs to load before executing tests.");
  case OPT_SLAP_TEST: return("Test to run (currently \"get\" or \"set\").");
  case OPT_SLAP_CONCURRENCY: return("Number of users to simulate with load.");
  case OPT_SLAP_NON_BLOCK: return("Set TCP up to use non-blocking IO.");
  case OPT_SLAP_TCP_NODELAY: return("Set TCP socket up to use nodelay.");
  case OPT_FLUSH: return("Flush servers before running tests.");
  case OPT_HASH: return("Select hash type.");
  case OPT_BINARY: return("Switch to binary protocol.");
  case OPT_ANALYZE: return("Analyze the provided servers.");
  case OPT_UDP: return("Use UDP protocol when communicating with server.");
  case OPT_USERNAME: return "Username to use for SASL authentication";
  case OPT_PASSWD: return "Password to use for SASL authentication";
  case OPT_FILE: return "Path to file in which to save result";
  case OPT_STAT_ARGS: return "Argument for statistics";
  default: WATCHPOINT_ASSERT(0);
  };

  WATCHPOINT_ASSERT(0);
  return "forgot to document this function :)";
}

void help_command(const char *command_name, const char *description,
                  const struct option *long_options,
                  memcached_programs_help_st *options __attribute__((unused)))
{
  unsigned int x;

  printf("%s v%u.%u\n\n", command_name, 1U, 0U);
  printf("\t%s\n\n", description);
  printf("Current options. A '=' means the option takes a value.\n\n");

  for (x= 0; long_options[x].name; x++)
  {
    const char *help_message;

    printf("\t --%s%c\n", long_options[x].name,
           long_options[x].has_arg ? '=' : ' ');
    if ((help_message= lookup_help(long_options[x].val)))
      printf("\t\t%s\n", help_message);
  }

  printf("\n");
  exit(0);
}

void process_hash_option(memcached_st *memc, char *opt_hash)
{
  uint64_t set;
  memcached_return_t rc;

  if (opt_hash == NULL)
    return;

  set= MEMCACHED_HASH_DEFAULT; /* Just here to solve warning */
  if (!strcasecmp(opt_hash, "CRC"))
    set= MEMCACHED_HASH_CRC;
  else if (!strcasecmp(opt_hash, "FNV1_64"))
    set= MEMCACHED_HASH_FNV1_64;
  else if (!strcasecmp(opt_hash, "FNV1A_64"))
    set= MEMCACHED_HASH_FNV1A_64;
  else if (!strcasecmp(opt_hash, "FNV1_32"))
    set= MEMCACHED_HASH_FNV1_32;
  else if (!strcasecmp(opt_hash, "FNV1A_32"))
    set= MEMCACHED_HASH_FNV1A_32;
  else
  {
    fprintf(stderr, "hash: type not recognized %s\n", opt_hash);
    exit(1);
  }

  rc= memcached_behavior_set(memc, MEMCACHED_BEHAVIOR_HASH, set);
  if (rc != MEMCACHED_SUCCESS)
  {
    fprintf(stderr, "hash: memcache error %s\n", memcached_strerror(memc, rc));
    exit(1);
  }
}
