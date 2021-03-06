#ifndef STATISTICS_H
#define STATISTICS_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timing.h"

typedef struct statistics{
  int num_sections;
  int num_subsections;
  pthread_mutex_t mutex;
  unsigned int *num_values;
  char **section_sublabels_p;
  char **section_labels_p;
  size_t **values_p;
}statistics_t;

extern char statistics_on;
extern statistics_t *statistics_p;

statistics_t* statistics_new(char** section_labels_p, char** section_sublabels_p, unsigned int *num_values, int num_sections, int num_subsection);

void statistics_free(statistics_t* statistics_p);

void statistics_set(unsigned int section, unsigned int subsection, size_t value, statistics_t* statistics_p);

void statistics_add(unsigned int section, unsigned int subsection, size_t value, statistics_t* statistics_p);

void statistics_display(statistics_t* statistics_p);

void timing_and_statistics_display(statistics_t* statistics_p, timing_t* timing_p);

#endif // end of if TIMING
