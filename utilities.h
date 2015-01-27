#ifndef __UTILITIES_H__
#define __UTILITIES_H__
//#include <getopt.h>
#include <sys/time.h>


long timedif_us(struct timeval end, struct timeval start);
unsigned long sort_ascend_int(int *array, int begin, int end);


#endif  // __UTILITIES_H__
