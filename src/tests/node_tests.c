#include "global.h"
#include "all_tests.h"
#include <ctype.h>

#include "db_graph.h"
#include "db_node.h"
#include "build_graph.h"

static void edge_check(hkey_t node, const dBGraph *db_graph, size_t col)
{
  const BinaryKmer bkmer = db_node_get_bkmer(db_graph, node);
  const Edges edges = db_node_get_edges(db_graph, col, node);

  dBNode nodes[4];
  Nucleotide nucs[4];
  size_t i, n, or;

  for(or = 0; or < 2; or++) {
    Edges e = 0;
    n = db_graph_next_nodes(db_graph, bkmer, or, edges, nodes, nucs);
    for(i = 0; i < n; i++) e |= nuc_orient_to_edge(nucs[i], or);
    assert(edges_with_orientation(e,or) == edges_with_orientation(edges,or));
  }
}

void test_db_node()
{
  test_status("[db_node] Testing db_graph_next_nodes vs edges_ functions");

  // Construct 2 colour graph with kmer-size=11
  dBGraph graph;
  size_t col, kmer_size = 11, ncols = 2;
  char seq[60];

  db_graph_alloc(&graph, kmer_size, ncols, ncols, 1024);
  graph.bktlocks = calloc2(roundup_bits2bytes(graph.ht.num_of_buckets), 1);
  graph.col_edges = calloc2(graph.ht.capacity * ncols, sizeof(Edges));
  graph.col_covgs = calloc2(graph.ht.capacity * ncols, sizeof(Covg));

  // Copy a random and shared piece of sequence to both colours
  for(col = 0; col < 2; col++) {
    dna_rand_str(seq, 60);
    build_graph_from_str_mt(&graph, col, seq, strlen(seq));
    strcpy(seq, "CTTTCTTATCTGGAACCAGCTTTGCGGGGATGGAGTGTAACCTTGACAATGGGTCCTGC");
    build_graph_from_str_mt(&graph, col, seq, strlen(seq));
  }

  HASH_ITERATE(&graph.ht, edge_check, &graph, 0);
  HASH_ITERATE(&graph.ht, edge_check, &graph, 1);

  free(graph.bktlocks);
  free(graph.col_covgs);
  free(graph.col_edges);
  db_graph_dealloc(&graph);
}