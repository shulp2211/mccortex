#include "global.h"

#include "seq_file.h"

#include "cmd.h"
#include "util.h"
#include "file_util.h"
#include "binary_kmer.h"
#include "hash_table.h"
#include "db_graph.h"
#include "binary_format.h"
#include "seq_reader.h"

static const char usage[] =
"usage: "CMD" extend [-m <mem>] <in.ctx> <in.fa> <dist> <out.fa>\n";

typedef struct
{
  dBGraph *db_graph;
  dBNodeBuffer *readbuffw, *readbufrv;
  FILE *out;
  char *buf;
  size_t buflen;
  size_t max_walk;
  uint64_t *visited;
} ExtendContig;

static void node_not_in_graph(BinaryKmer bkmer, dBGraph *db_graph)
__attribute__((noreturn));

static void node_not_in_graph(BinaryKmer bkmer, dBGraph *db_graph)
{
  char str[MAX_KMER_SIZE+1];
  binary_kmer_to_str(bkmer, db_graph->kmer_size, str);
  die("Node not in graph: %s", str);
}

static void add_kmer(BinaryKmer bkmer, dBGraph *db_graph, dBNodeBuffer *nodebuf)
{
  BinaryKmer bkey;
  db_node_get_key(bkmer, db_graph->kmer_size, bkey);
  hkey_t node = hash_table_find(&db_graph->ht, bkey);
  if(node == HASH_NOT_FOUND) node_not_in_graph(bkmer, db_graph);
  Orientation orient = db_node_get_orientation(bkmer, bkey);
  nodebuf->data[nodebuf->len].node = node;
  nodebuf->data[nodebuf->len].orient = orient;
  nodebuf->len++;
}

static void walk_graph(hkey_t node, Orientation orient,
                       dBNodeBuffer *buf, uint32_t max,
                       dBGraph *db_graph, uint64_t *visited)
{
  uint32_t i, origlen = buf->len;
  Edges edges;
  Nucleotide nuc;
  BinaryKmer bkmer, bkey;

  for(i = 0; i < max; i++)
  {
    edges = db_node_edges(db_graph, node);

    if(!edges_has_precisely_one_edge(edges, orient, &nuc)) break;

    db_graph_next_node_orient(db_graph, db_node_bkmer(db_graph, node),
                              nuc, orient, &node, &orient);
    db_node_get_key(bkmer, db_graph->kmer_size, bkey);
    node = hash_table_find(&db_graph->ht, bkey);
    orient = db_node_get_orientation(bkmer, bkey);
    if(node == HASH_NOT_FOUND) node_not_in_graph(bkmer, db_graph);
    if(db_node_has_traversed(visited, node, orient)) break;
    db_node_set_traversed(visited, node, orient);

    buf->data[buf->len].node = node;
    buf->data[buf->len].orient = orient;
    buf->len++;
  }

  for(i = origlen; i < buf->len; i++)
    db_node_fast_clear_traversed(visited, buf->data[i].node);
}

static void extend_read(read_t *r, ExtendContig *contig, SeqLoadingStats *stats)
{
  dBGraph *db_graph = contig->db_graph;
  dBNodeBuffer *readbuffw = contig->readbuffw, *readbufrv = contig->readbufrv;

  if(r->seq.end < db_graph->kmer_size) return;

  db_node_buf_ensure_capacity(readbuffw, r->seq.end + contig->max_walk * 2);
  readbuffw->len = readbufrv->len = 0;
  READ_TO_BKMERS(r, db_graph->kmer_size, 0, 0, stats, add_kmer, db_graph, readbuffw);

  if(readbuffw->len == 0) die("Invalid entry: %s", r->name.b);

  size_t i, j;

  // extend forwards and backwards
  walk_graph(readbuffw->data[readbuffw->len-1].node,
             readbuffw->data[readbuffw->len-1].orient,
             readbuffw, contig->max_walk, db_graph, contig->visited);

  walk_graph(readbuffw->data[0].node,
             opposite_orientation(readbuffw->data[0].orient),
             readbufrv, contig->max_walk, db_graph, contig->visited);

  // Shift forward list up
  memmove(readbuffw->data+readbufrv->len, readbuffw->data,
          readbuffw->len * sizeof(dBNode));

  // Reverse orientation for backwards kmers
  for(i = 0, j = readbufrv->len-1; i < readbufrv->len; i++, j--) {
    readbuffw->data[i].node = readbufrv->data[j].node;
    readbuffw->data[i].orient = opposite_orientation(readbufrv->data[j].orient);
  }

  // to string and print
  size_t len = readbuffw->len + readbufrv->len;
  if(contig->buflen < len+1) {
    contig->buflen = ROUNDUP2POW(len+1);
    contig->buf = realloc(contig->buf, contig->buflen);
  }

  db_nodes_to_str(readbuffw->data, len, db_graph, contig->buf);

  fprintf(contig->out, ">%s\n%s\n", r->name.b, contig->buf);
}

static void extend_reads(read_t *r1, read_t *r2,
                         int qoffset1, int qoffset2,
                         SeqLoadingPrefs *prefs,
                         SeqLoadingStats *stats, void *ptr)
{
  (void)qoffset1; (void)qoffset2; (void)prefs;
  ExtendContig *contig = (ExtendContig*)ptr;
  extend_read(r1, contig, stats);
  if(r2 != NULL) extend_read(r2, contig, stats);
}

int ctx_extend(CmdArgs *args)
{
  cmd_accept_options(args, "m");
  cmd_require_options(args, "m");
  int argc = args->argc;
  char **argv = args->argv;
  if(argc != 4) print_usage(usage, NULL);

  size_t mem_to_use = args->mem_to_use;

  char *input_ctx_path, *input_fa_path, *out_fa_path;
  uint32_t dist;

  input_ctx_path = argv[2];
  input_fa_path = argv[3];

  // Probe binary
  boolean is_binary = false;
  uint32_t kmer_size, num_of_cols, max_col;
  uint64_t num_kmers;

  if(!binary_probe(input_ctx_path, &is_binary, &kmer_size, &num_of_cols, &max_col, &num_kmers))
    print_usage(usage, "Cannot read binary file: %s", input_ctx_path);
  else if(!is_binary)
    print_usage(usage, "Input binary file isn't valid: %s", input_ctx_path);

  if(!test_file_readable(input_fa_path))
    print_usage(usage, "Cannot read input FASTA/FASTQ/SAM/BAM file: %s", input_fa_path);

  if(!parse_entire_uint(argv[4], &dist))
    print_usage(usage, "Invalid dist argument: %s", argv[4]);

  out_fa_path = argv[5];

  if(!test_file_writable(out_fa_path))
    print_usage(usage, "Cannot write output file: %s", out_fa_path);

  // Decide on memory
  size_t hash_kmers, req_num_kmers = num_kmers*(1.0/IDEAL_OCCUPANCY);
  size_t hash_mem = hash_table_mem(req_num_kmers, &hash_kmers);

  size_t graph_mem = hash_mem +
                     hash_kmers * sizeof(Edges) +
                     2*round_bits_to_words64(hash_kmers)*sizeof(uint64_t);

  char graph_mem_str[100];
  bytes_to_str(graph_mem, 1, graph_mem_str);

  if(graph_mem > mem_to_use)
    print_usage(usage, "Not enough memory - binary requires: %s", graph_mem_str);

  message("[memory] graph: %s\n", graph_mem_str);
  message("Using kmer size: %u\n", kmer_size);
  message("Max walk: %u\n", dist);

  // Set up dBGraph
  dBGraph db_graph;
  db_graph_alloc(&db_graph, kmer_size, num_of_cols, hash_kmers);
  db_graph.edges = calloc(db_graph.ht.capacity, sizeof(Edges));

  size_t visited_words = 2*round_bits_to_words64(db_graph.ht.capacity);
  uint64_t *visited = calloc(visited_words, sizeof(Edges));

  if(db_graph.edges == NULL || visited == NULL) die("Out of memory");

    // Store edge nodes here
  dBNodeBuffer readbuffw, readbufrv;
  db_node_buf_alloc(&readbuffw, 2048);
  db_node_buf_alloc(&readbufrv, 1024);

  size_t buflen = 1024;
  char *buf = malloc(buflen * sizeof(char));

  FILE *out = fopen(out_fa_path, "w");
  if(out == NULL) die("Cannot open output file: %s", out_fa_path);

  if(buf == NULL) die("Out of memory");

  SeqLoadingStats *stats = seq_loading_stats_create(0);
  SeqLoadingPrefs prefs = {.into_colour = 0, .merge_colours = true,
                           .load_seq = false,
                           .quality_cutoff = 0, .ascii_fq_offset = 0,
                           .homopolymer_cutoff = 0,
                           .remove_dups_se = false, .remove_dups_pe = false,
                           .load_binaries = true,
                           .must_exist_in_colour = -1,
                           .empty_colours = false,
                           .update_ginfo = false,
                           .db_graph = &db_graph};

  // Load binary
  binary_load(input_ctx_path, &db_graph, &prefs, stats, NULL);

  prefs.load_seq = true;
  prefs.load_binaries = false;

  ExtendContig contig = {.db_graph = &db_graph,
                         .readbuffw = &readbuffw, .readbufrv = &readbufrv,
                         .out = out, .buf = buf, .buflen = buflen,
                         .max_walk = dist, .visited = visited};

  // Parse sequence
  read_t r1, r2;
  seq_read_alloc(&r1);
  seq_read_alloc(&r2);
  seq_parse_se(input_fa_path, &r1, &r2, &prefs, stats, extend_reads, &contig);
  seq_read_dealloc(&r1);
  seq_read_dealloc(&r2);

  message("Done.\n");

  fclose(out);

  free(buf);
  db_node_buf_dealloc(&readbuffw);
  db_node_buf_dealloc(&readbufrv);
  seq_loading_stats_free(stats);
  free(visited);
  free(db_graph.edges);
  db_graph_dealloc(&db_graph);

  return EXIT_SUCCESS;
}
