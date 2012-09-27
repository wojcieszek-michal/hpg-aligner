#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <omp.h>

#include "commons/log.h"

#include "bioformats/fastq/fastq_batch_reader.h"

#include "error.h"

#include "timing.h"
#include "buffers.h"
#include "bwt_server.h"
#include "batch_writer.h"
#include "region_seeker.h"
#include "cal_seeker.h"
#include "sw_server.h"
#include "pair_server.h"
#include "batch_aligner.h"

// rna server
#include "rna_server.h"
#include "sw.h"
#include "smith_waterman.h"

#include "options.h"
#include "statistics.h"


double emboss_matrix_t = 0.0f, emboss_tracking_t = 0.0f;
double sse_matrix_t = 0.0f, sse_tracking_t = 0.0f;
double sse1_matrix_t = 0.0f, sse1_tracking_t = 0.0f;
double avx_matrix_t = 0.0f, avx_tracking_t = 0.0f;
double avx1_matrix_t = 0.0f, avx1_tracking_t = 0.0f;

//-------------------------------------------------------
// constants
//-------------------------------------------------------

#define OPTIONS 			30
#define MIN_ARGC  			5
#define NUM_SECTIONS_TIME 		10
#define NUM_SECTIONS_STATISTICS 	6
#define NUM_SECTIONS_STATISTICS_SB	23
//#define REQUIRED 1
//#define NO_REQUIRED 0

//--------------------------------------------------------------------
// global variables for log functions
//--------------------------------------------------------------------

//int log_level = DEFAULT_LOG_LEVEL;
//bool verbose = true;

//--------------------------------------------------------------------
// global variables for timing and capacity meausures
//--------------------------------------------------------------------

char time_on = 0;
char statistics_on = 0;
timing_t* timing_p = NULL;
int num_of_chromosomes;
statistics_t *statistics_p;

//--------------------------------------------------------------------
// main parameters support
//--------------------------------------------------------------------

int main(int argc, char* argv[]) {
  options_t *options_p = parse_options(argc, argv);

  char* in_filename = strdup(options_p->in_filename);
  char* bwt_dirname =  strdup(options_p->bwt_dirname);
  char* genome_filename =  strdup(options_p->genome_filename);
  char* chromosome_filename =  strdup(options_p->chromosome_filename);
  char* output_filename =  strdup(options_p->output_filename);
  unsigned int num_gpu_threads =  (unsigned int)options_p->num_gpu_threads;
  unsigned int num_cpu_threads =  (unsigned int)options_p->num_cpu_threads;
  unsigned int rna_seq =  (unsigned int)options_p->rna_seq; 
  unsigned int cal_seeker_errors =  (unsigned int)options_p->cal_seeker_errors; 
  unsigned int min_cal_size =  (unsigned int)options_p->min_cal_size; 
  unsigned int seeds_max_distance =  (unsigned int)options_p->seeds_max_distance; 
  unsigned int bwt_threads =  (unsigned int)options_p->bwt_threads; 
  unsigned int batch_size =  (unsigned int)options_p->batch_size; 
  unsigned int write_size =  (unsigned int)options_p->write_size;  
  unsigned int num_cal_seekers =  (unsigned int)options_p->num_cal_seekers;
  unsigned int region_threads =  (unsigned int)options_p->region_threads;
  unsigned int num_sw_servers =  (unsigned int)options_p->num_sw_servers;
  unsigned int min_seed_size =  (unsigned int)options_p->min_seed_size;
  unsigned int seed_size =  (unsigned int)options_p->seed_size;
  unsigned int max_intron_length =  (unsigned int)options_p->max_intron_length;
  unsigned int flank_length =  (unsigned int)options_p->flank_length;
  float min_score =  (float)options_p->min_score;
  float match =   (float)options_p->match;
  float mismatch =   (float)options_p->mismatch;
  float gap_open =   (float)options_p->gap_open;
  float gap_extend =   (float)options_p->gap_extend;
  unsigned int version;
  char *splice_exact_filename =  strdup(options_p->splice_exact_filename);
  char *splice_extend_filename =  strdup(options_p->splice_extend_filename);
  unsigned int min_intron_length =  options_p->min_intron_length;
  time_on =  (unsigned int)options_p->timming;
  statistics_on =  (unsigned int)options_p->statistics;
  
  options_free(options_p);
  
  register int i;

  printf("PARAMETERS CONFIGURATION\n");
  printf("=================================================\n");
  printf("Num gpu threads %d\n", num_gpu_threads);
  printf("Num cpu threads %d\n",  num_cpu_threads);
  printf("RNA Server: %s\n",  rna_seq == 0 ? "Disable":"Enable");
  printf("CAL seeker errors: %d\n",  cal_seeker_errors);
  printf("Min CAL size: %d\n",  min_cal_size);
  printf("Seeds max distance: %d\n",  seeds_max_distance);
  printf("Batch size: %dBytes\n",  batch_size);
  printf("Write size: %dBytes\n",  write_size);
  printf("BWT Threads: %d\n",  bwt_threads);
  printf("Region Threads: %d\n",  region_threads);
  printf("Num CAL seekers: %d\n", num_cal_seekers);
  printf("Num SW servers: %d\n",  num_sw_servers);
  printf("Min seed size: %d\n",  min_seed_size);
  printf("Seed size: %d\n",  seed_size);
  printf("Max intron length: %d\n", max_intron_length);
  printf("Min intron length: %d\n", min_intron_length);
  printf("Flank length: %d\n", flank_length);
  printf("SMITH-WATERMAN PARAMETERS\n");
  printf("\tMin score  : %0.4f\n", min_score);
  printf("\tMatch      : %0.4f\n", match);
  printf("\tMismatch   : %0.4f\n", mismatch);
  printf("\tGap open   : %0.4f\n", gap_open);
  printf("\tGap extend : %0.4f\n", gap_extend);
  printf("=================================================\n");

  // timing
  if (time_on) { 
    char* labels_time[NUM_SECTIONS_TIME] = {"Initialization BWT index   ", 
					    "Initialization genome index", 
					    "FastqQ reader              ", 
					    "BWT server                 ", 
					    "Region Seeker              ", 
					    "CAL seeker                 ", 
					    "Rna server                 ", 
					    "Batch writer               ", 
					    "Free main memory           ", 
					    "Total time                 "};
    
    int num_threads[NUM_SECTIONS_TIME] = {1, 1, 1, 1, 1, num_cal_seekers, num_sw_servers, 1, 1, 1};
    timing_p = timing_new((char**) labels_time, (int*) num_threads, NUM_SECTIONS_TIME);
    
    timing_start(MAIN_INDEX, 0, timing_p);
    //timing_start(INIT_INDEX, 0, timing_p);
    
  }

  if (statistics_on) {
    /*
     * FastqQ reader      : Num Batches %d 
     * BWT server         : Num Reads process %d, Num reads mapped %d, Num reads unmapped %d, Num mappings report %d 
     * Region Seeker      : Total num regions %d
     * CAL seeker         : Total cals %d, Num Reads unmapped %d
     * Rna server         : Num reads mapped %d, Num reads unmapped %d 
     * Batch writer       : Total num mappings report %d
     * Total 		  : Total reads %d, Total reads mapped %d, Total reads unmapped %d, Total splice junctions %d 	
     */				
    char* labels_statistics[NUM_SECTIONS_STATISTICS] = {"FastqQ reader              ", 
							"BWT server                 ", 
							"Region seeker              ", 
							"CAL seeker                 ", 
							"Rna server                 ",
							"Total 	Statistics	    "};
    
    char *sub_labels_statistics[NUM_SECTIONS_STATISTICS_SB] = { "Num batches             ",
								"Total reads             ",
								
								"Num batches process     ",
								"Reads process           ",
								"   Reads mapped         ", 
								"   Reads unmapped       ",
								"Mappings report         ",
								
								"Num batches process     ",
								"Num reads process       ",
								
								"Num batches process     ",
								"Total reads process     ",

								"Num reads unmapped      ",
								
								"Num batches process     ",
								"Num reads               ",
								"Num reads mapped        ",
								"Num reads unmapped      ",
								"Num sw process          ",
								"   Num sw valids        ",
								"   Num sw no valids     ",
								
								"Total reads             ",
								"   Total reads mapped   ",
								"   Total reads unmapped ",
								"Total splice junctions  "
								};
				      
    unsigned int num_values[NUM_SECTIONS_STATISTICS] = {2, 5, 2, 3, 7, 4};
    statistics_p = statistics_new((char **)labels_statistics, (char **)sub_labels_statistics, (unsigned int *)num_values, NUM_SECTIONS_STATISTICS, NUM_SECTIONS_STATISTICS_SB);
    
  }
  // initialize some structures: Burrow-Wheeler objects, genome, nucletotide table...
  // (all these initializations could be performed in parallel)
  //

  printf("Reading bwt index...\n");

  if(time_on){ timing_start(INIT_BWT_INDEX, 0, timing_p); }
  bwt_index_t *bwt_index_p = bwt_index_new(bwt_dirname);
  if(time_on){ timing_stop(INIT_BWT_INDEX, 0, timing_p); }
  printf("Reading bwt index done !!\n");
  // (errors, threads, max aligns) 
  bwt_optarg_t *bwt_optarg_p = bwt_optarg_new(1, bwt_threads, 500);
  
  
  //GOOD LUCK(20, 60, 18, 16, 0)
  //                  cal_optarg_new(min_cal_size, max_cal_distance, seed_size, min_seed_size, num_error)
  cal_optarg_t *cal_optarg_p = cal_optarg_new(min_cal_size, seeds_max_distance, seed_size, min_seed_size, cal_seeker_errors);
  
  printf("reading genome...\n");

  if(time_on){ timing_start(INIT_GENOME_INDEX, 0, timing_p); }
  genome_t* genome_p = genome_new(genome_filename, chromosome_filename);
  if(time_on){ timing_stop(INIT_GENOME_INDEX, 0, timing_p); }
  printf("reading genome done !!\n");

  /*
  {
    //@6_13362459_13363092_0_1_0_0_3:0:0_2:0:0_13195/1
    //@3_17488330_17489008_0_1_0_0_3:0:0_1:0:0_7101/1
    char seq[1000];
    int chr = 3;
    int strand = 0;
    size_t start = 17488330;
    size_t end = 17488429;
    genome_read_sequence_by_chr_index(seq, strand, chr - 1, &start, &end, genome_p);
    printf("%s\n", seq);
    exit(-1);
  }
  */

  printf("init table...\n");
  initTable();
  printf("init table done !!\n");

  void *context_p = NULL;

 /* if (cuda) {
    int num_gpus = 2;
    context_p = (void *) gpu_context_new(num_gpus, num_gpu_threads);
  }*/
  
  // lists to communicate/synchronize the different threads
  //
  /* >>>>>> JT
  list_t read_list;
  list_t unmapped_reads_list;
  list_t regions_list;
  list_t sw_list; 
  list_t pair_list;
  list_t write_list;

  list_init("read", 1, 5, &read_list);
  list_init("unmmaped reads", 1, 5, &unmapped_reads_list);
  list_init("regions", 1, 5, &regions_list);
  
  if (is_pair) {
    list_init("sw", num_cal_seekers, 5, &sw_list);
    list_init("pair", 1 + num_cal_seekers, 5, &pair_list);
    list_init("write", 1 + num_sw_servers, 5, &write_list);

    pair_mng = pair_mng_new(pair_mode, pair_min_distance,
			    pair_max_distance);
  } else {
    list_init("sw", num_cal_seekers, 5, &sw_list); 
    list_init("pair", 0, 5, &pair_list);
    list_init("write", 1 + num_sw_servers + num_cal_seekers, 
	      5, &write_list);
  }

  // in sw_list, the threads insert are the cal_seekers and
  // in case of pair mode, the bwt_server too

  //start delete ...
  //list_decr_writers(&unmapped_reads_list);
  //list_decr_writers(&regions_list);
  //list_decr_writers(&sw_list);
  //list_decr_writers(&write_list);
  //list_decr_writers(&write_list);
  //list_decr_writers(&write_list);
  //end delete
  <<<<<<<<<<<<<<<  JT */

  list_t read_list;
  list_init("read", 1, 10, &read_list);
  
  list_t unmapped_reads_list;
  list_init("unmmaped reads", 1, 10, &unmapped_reads_list);
  
  list_t regions_list;
  list_init("regions", 1, 10, &regions_list);
  
  list_t sw_list;
  list_init("sw", num_cal_seekers, 10, &sw_list);

  list_t write_list;
  list_init("write", 1 + num_cal_seekers + num_sw_servers, 10, &write_list);
  

  // launch threads in parallel
  //
  omp_set_nested(1);
  //omp_set_num_threads(num_cpu_threads);
  
  //Rna Version
  /* >>>>>> JT
  version = 2;


  for (int i = 0; i < num_cpu_threads; i++) {
    unmapped_by_max_cals_counter[i] = 0;
    unmapped_by_score_counter[i] = 0;
  }

  //*********************************************************************
  //*********************************************************************
  //*********************************************************************
  {

    list_t read_list, write_list;
    list_init("read", 1, 24, &read_list);
    list_init("write", num_cpu_threads, 24, &write_list);

    printf("writers to read_list = %d\n", list_get_writers(&read_list));
    printf("writers to write_list = %d\n", list_get_writers(&write_list));
    
    bwt_server_input_t bwt_input;
    bwt_server_input_init(NULL, 0, bwt_optarg_p, bwt_index_p, 
			  NULL, NULL, 0, NULL, &bwt_input);
    
    region_seeker_input_t region_input;
    region_seeker_input_init(NULL, cal_optarg_p, bwt_optarg_p, 
			     bwt_index_p, NULL, &region_input);
    
    cal_seeker_input_t cal_input;
    cal_seeker_input_init(NULL, cal_optarg_p, NULL, 0, 
			  NULL, NULL, &cal_input);
    
    pair_server_input_t pair_input;
    pair_server_input_init(pair_mng, NULL, NULL, NULL, &pair_input);
    
    sw_server_input_t sw_input;
    sw_server_input_init(NULL, NULL, 0, match, mismatch, 
			 gap_open, gap_extend, min_score, flank_length, genome_p, 
			 &sw_input);


    
    double t_total;
    struct timeval t1, t2;

    #pragma omp parallel sections num_threads(3 + num_cpu_threads)
    {
      printf("Principal Sections %d threads\n", omp_get_num_threads());

      // fastq batch reader
      #pragma omp section
      {
	fastq_batch_reader_input_t input;
	fastq_batch_reader_input_init(in_filename1, in_filename2, flags, batch_size, 
				      &read_list, &input);
	fastq_batch_reader(&input);
      }

      // batch aligner
      #pragma omp section
      {
	gettimeofday(&t1, NULL);
        #pragma omp parallel num_threads(num_cpu_threads)
	{
	  batch_aligner_input_t input;
	  batch_aligner_input_init(&read_list, &write_list,
				   &bwt_input, &region_input, &cal_input,
				   (is_pair ? &pair_input : NULL), &sw_input,
				   &input);
	  
	  batch_aligner(&input);
	}
	gettimeofday(&t2, NULL);
	printf("\n\naligner time (no IO): %0.5f sec\n\n", (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1e6);
      }
      // batch writer
      #pragma omp section
      {
	batch_writer_input_t input;
	batch_writer_input_init("new.sam", splice_filename, &write_list, &input);
	batch_writer2(&input);
      }
    }
  }

//  printf("main.c: aborting by debugging\n");
//  abort();

  size_t total_item = 0;
  double max_time = 0, total_throughput = 0;
  printf("\nBWT time:\n");
  for (int i = 0; i < num_cpu_threads; i++) {
    printf("\tThread %d: %0.4f s (%d batches, %d items) -> %0.2f BWT/s (reads)\n", 
	   i, bwt_time[i] / 1e6, thr_batches[i], thr_bwt_items[i], 1e6 * thr_bwt_items[i] / bwt_time[i]);
    total_item += thr_bwt_items[i];
    total_throughput += (1e6 * thr_bwt_items[i] / bwt_time[i]);
    if (max_time < bwt_time[i]) max_time = bwt_time[i];
  }
  printf("\n\tTotal BWTs: %d, Max time = %0.4f, Throughput = %0.2f BWT/s\n", total_item, max_time / 1e6, total_throughput);

  total_item = 0; max_time = 0; total_throughput = 0;
  printf("\nSeeding time:\n");
  for (int i = 0; i < num_cpu_threads; i++) {
    printf("\tThread %d: %0.4f s (%d batches, %d items) -> %0.2f BWT/s (seeds)\n", 
	   i, seeding_time[i] / 1e6, thr_batches[i], thr_seeding_items[i], 1e6 * thr_seeding_items[i] / seeding_time[i]);
    total_item += thr_seeding_items[i];
    total_throughput += (1e6 * thr_seeding_items[i] / seeding_time[i]);
    if (max_time < seeding_time[i]) max_time = seeding_time[i];
  }
  printf("\n\tTotal BWTs: %d, Max time = %0.4f, Throughput = %0.2f BWT/s\n", total_item, max_time / 1e6, total_throughput);

  total_item = 0; max_time = 0; total_throughput = 0;
  printf("\nCAL time:\n");
  for (int i = 0; i < num_cpu_threads; i++) {
    printf("\tThread %d: %0.4f s (%d batches, %d items) -> %0.2f CAL/s)\n", 
	   i, cal_time[i] / 1e6, thr_batches[i], thr_cal_items[i], 1e6 * thr_cal_items[i] / cal_time[i]);
    total_item += thr_cal_items[i];
    total_throughput += (1e6 * thr_cal_items[i] / cal_time[i]);
    if (max_time < cal_time[i]) max_time = cal_time[i];
  }
  printf("\n\tTotal CALs: %d, Max time = %0.4f, Throughput = %0.2f CAL/s\n", total_item, max_time / 1e6, total_throughput);

  total_item = 0; max_time = 0; total_throughput = 0;
  printf("\nSW time:\n");
  for (int i = 0; i < num_cpu_threads; i++) {
    printf("\tThread %d: %0.4f s (%d batches, %d items) -> %0.2f SW/s)\n", 
	   i, sw_time[i] / 1e6, thr_batches[i], thr_sw_items[i], 1e6 * thr_sw_items[i] / sw_time[i]);
    total_item += thr_sw_items[i];
    total_throughput += (1e6 * thr_sw_items[i] / sw_time[i]);
    if (max_time < sw_time[i]) max_time = sw_time[i];
  }
  printf("\n\tTotal SWs: %d, Max time = %0.4f, Throughput = %0.2f SW/s\n", total_item, max_time / 1e6, total_throughput);

  //*********************************************************************
  //*********************************************************************
  //*********************************************************************

    // free memory
    bwt_optarg_free(bwt_optarg_p);
    cal_optarg_free(cal_optarg_p);
    bwt_index_free(bwt_index_p);

    if (pair_mng != NULL) pair_mng_free(pair_mng);
    if (time_on) { timing_start(FREE_MAIN, 0, timing_p); }
    genome_free(genome_p);
    if (time_on) { timing_stop(FREE_MAIN, 0, timing_p); }
    
    //if (cuda) {
   //   gpu_context_free((gpu_context_t*) context_p);
   //   }
   //   if (time_on) { timing_stop(FREE_INDEX, 0, timing_p); }
    
    if (time_on) { 
      timing_stop(MAIN_INDEX, 0, timing_p);
      timing_display(timing_p);
      timing_free(timing_p);
    }
    
    if(statistics_on) {
      statistics_display(statistics_p);
      statistics_free(statistics_p);
    }


    int by_cals = 0, by_score = 0;
    for (int i = 0; i < num_cpu_threads; i++) {
      by_cals += unmapped_by_max_cals_counter[i];
      by_score += unmapped_by_score_counter[i];
    }
    printf("unmapped by MAX_CALS = %d\n", by_cals);
    printf("unmapped by SW score = %d\n", by_score);
    
    return 0;
  <<<<<<<<<<<<<<<  JT */

  #pragma omp parallel sections num_threads(6)
  {
    printf("Principal Sections %d threads\n", omp_get_num_threads());
    #pragma omp section
    {
      fastq_batch_reader_input_t input;
      fastq_batch_reader_input_init(in_filename, batch_size, &read_list, &input);
      fastq_batch_reader(&input);
    }
    #pragma omp section
    {
	bwt_server_input_t input;
        bwt_server_input_init(&read_list, batch_size, bwt_optarg_p, bwt_index_p, &write_list, write_size, &unmapped_reads_list, &input);
	bwt_server_cpu(&input);
    }
    #pragma omp section
    {
	region_seeker_input_t input;
	region_seeker_input_init(&unmapped_reads_list, cal_optarg_p, bwt_optarg_p, bwt_index_p, &regions_list, region_threads, &input);
	region_seeker_server(&input);
    }
    #pragma omp section
    {
      cal_seeker_input_t input;
      cal_seeker_input_init(&regions_list, cal_optarg_p, &write_list, write_size, &sw_list, &input);
      #pragma omp parallel num_threads(num_cal_seekers)
      {
	cal_seeker_server(&input);
      }
      
    }
    #pragma omp section
    {
      allocate_splice_elements_t chromosome_avls[CHROMOSOME_NUMBER];
      init_allocate_splice_elements(&chromosome_avls);
      sw_server_input_t input;
      sw_server_input_init(&sw_list, &write_list, write_size, match, mismatch, gap_open, 
			   gap_extend, min_score, flank_length, genome_p, max_intron_length,
			   min_intron_length, seeds_max_distance, &input);
      list_incr_writers(&write_list);
      #pragma omp parallel num_threads(num_sw_servers)
      {
	if (rna_seq){
	  rna_server_omp_smith_waterman(&input, &chromosome_avls);
	}else {
	  sw_server(&input);
	} 
      }

      if(rna_seq){
	process_and_free_chromosome_avls(&chromosome_avls, &write_list, write_size);
      }

      }
     #pragma omp section
    {
      batch_writer_input_t input;
      batch_writer_input_init(output_filename, splice_exact_filename, splice_extend_filename, &write_list, &input);
      batch_writer(&input);
    }
  }
  
  printf("\nmain done !!\n");

  // free memory
  //  
  if (time_on) { timing_start(FREE_MAIN, 0, timing_p); }
  
  bwt_index_free(bwt_index_p);
  genome_free(genome_p);
  bwt_optarg_free(bwt_optarg_p);
  cal_optarg_free(cal_optarg_p);

  free(in_filename);
  free(bwt_dirname);
  free(genome_filename);
  free(chromosome_filename);
  free(output_filename);
  free(splice_exact_filename);
  free(splice_extend_filename);
  
  if (time_on) { timing_stop(FREE_MAIN, 0, timing_p); }
  
  /*if (cuda) {
    gpu_context_free((gpu_context_t*) context_p);
  }
  if (time_on) { timing_stop(FREE_INDEX, 0, timing_p); }*/

  if (time_on) { 
    timing_stop(MAIN_INDEX, 0, timing_p);
    timing_display(timing_p);
  }
  
  if (statistics_on) { statistics_display(statistics_p); }
  
  if (statistics_on && time_on) { timing_and_statistics_display(statistics_p, timing_p); }

  if (time_on){ timing_free(timing_p); }

  if (statistics_on) { statistics_free(statistics_p); }

  return 0;
}


//-----------------------------------------------------
//-----------------------------------------------------