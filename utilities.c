#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <time.h>

#include "utilities.h"

unsigned long TimeNowInUs() {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

long timedif_us(struct timeval end, struct timeval start) {
  long s = end.tv_sec - start.tv_sec;
  return s * 1000000 + (end.tv_usec - start.tv_usec);
}


inline void swap_int(int *a, int *b) {
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

