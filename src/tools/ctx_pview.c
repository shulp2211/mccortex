#include "global.h"

#include "cmd.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "add_read_paths.h"
#include "binary_format.h"
#include "path_format.h"
#include "graph_walker.h"
#include "bubble_caller.h"

static const char usage[] = "usage: "CMD" pview [options] <in.ctx>\n";

int ctx_pview(CmdArgs *args)
{
  cmd_accept_options(args, "m");
  // cmd_require_options(args, "m", usage);
  int argc = args->argc;
  char **argv = args->argv;
  if(argc != 1) print_usage(usage, NULL);

  char *input_ctx_path = argv[0];

  // probe paths file to get kmer size
  char *input_paths_file = malloc2(strlen(input_ctx_path)+4);
  paths_format_filename(input_ctx_path, input_paths_file);
  boolean valid_paths_file = false;
  uint64_t ctp_num_paths = 0, ctp_num_path_bytes = 0, ctp_num_path_kmers = 0;
  uint32_t ctp_kmer_size = 0, ctp_num_of_cols = 0;

  if(!paths_format_probe(input_paths_file, &valid_paths_file,
                         &ctp_kmer_size, &ctp_num_of_cols, &ctp_num_paths,
                         &ctp_num_path_bytes, &ctp_num_path_kmers))
  {
    print_usage(usage, "Cannot find .ctp file: %s", input_paths_file);
  }

  if(!valid_paths_file)
    die("Invalid .ctp file: %s", input_paths_file);

  // Decide on memory
  size_t kmers_in_hash, ideal_capacity = ctp_num_path_kmers*(1.0/IDEAL_OCCUPANCY);
  size_t req_num_kmers = args->num_kmers_set ? args->num_kmers : ideal_capacity;
  size_t hash_mem = hash_table_mem(req_num_kmers, &kmers_in_hash);

  size_t graph_mem = hash_mem +
                     kmers_in_hash * sizeof(uint64_t); // kmer_paths

  size_t path_mem = args->mem_to_use - graph_mem;

  char num_kmers_str[100];
  ulong_to_str(ctp_num_path_kmers, num_kmers_str);

  char graph_mem_str[100], path_mem_str[100];
  bytes_to_str(graph_mem, 1, graph_mem_str);
  bytes_to_str(path_mem, 1, path_mem_str);

  message("[memory]  graph: %s;  paths: %s\n", graph_mem_str, path_mem_str);

  if(kmers_in_hash < ctp_num_path_kmers) {
    print_usage(usage, "Not enough kmers in the hash, require: %s "
                       "(set bigger -h <kmers> or -m <mem>)", num_kmers_str);
  }
  else if(kmers_in_hash < ideal_capacity)
    warn("Low memory for binary size (require: %s)", num_kmers_str);

  if(args->mem_to_use_set && graph_mem > args->mem_to_use)
    die("Not enough memory (please increase -m <mem>)");

  // Allocate memory
  // db graph is required to store the end position for each kmer list
  dBGraph db_graph;
  db_graph_alloc(&db_graph, ctp_kmer_size, 1, kmers_in_hash);

  // Paths
  db_graph.kmer_paths = malloc2(kmers_in_hash * sizeof(uint64_t));
  memset((void*)db_graph.kmer_paths, 0xff, kmers_in_hash * sizeof(uint64_t));

  uint8_t *path_store = malloc2(path_mem);
  binary_paths_init(&db_graph.pdata, path_store, path_mem, ctp_num_of_cols);

  // Pretend we've read all the kmers in
  db_graph.num_of_cols_used = ctp_num_of_cols;

  // Add kmers as reading
  paths_format_read(&db_graph, &db_graph.pdata, NULL, true, input_paths_file);

  db_graph_dump_paths_by_kmer(&db_graph);

  free(path_store);
  free((void *)db_graph.kmer_paths);

  db_graph_dealloc(&db_graph);
  free(input_paths_file);

  message("Done.\n");
  return EXIT_SUCCESS;
}
