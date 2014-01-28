#include "global.h"
#include "db_graph.h"
#include "db_node.h"
#include "graph_paths.h"
#include "packed_path.h"
#include "bit_macros.h"

// 1) check node after node has indegree >1 in sample ctxcol
// 2) follow path, check each junction matches up with a node with outdegree >1
// col is graph colour
// packed is just <PackedBases>
void graph_path_check_valid(dBNode node, size_t col, const uint8_t *packed,
                            size_t nbases, const dBGraph *db_graph)
{
  assert(db_graph->num_edge_cols == db_graph->num_of_cols ||
         db_graph->node_in_cols != NULL);

  BinaryKmer bkmer;
  Edges edges;
  hkey_t nodes[4];
  Orientation orients[4];
  Nucleotide nucs[4];
  size_t i, j, n, edgecol = db_graph->num_edge_cols > 1 ? col : 0;
  // length is kmers and juctions
  size_t klen, plen;

  // char nstr[nbases+1];
  // for(i = 0; i < nbases; i++) nstr[i] = dna_nuc_to_char(bases[i]);
  // nstr[nbases] = '\0';
  // status("Check: %zu:%i len %zu %s", (size_t)node.key, node.orient, nbases, nstr);

  for(klen = 0, plen = 0; plen < nbases; klen++)
  {
    bkmer = db_node_get_bkmer(db_graph, node.key);
    edges = db_node_get_edges(db_graph, edgecol, node.key);

    // Check this node is in this colour
    if(db_graph->node_in_cols != NULL) {
      assert(db_node_has_col(db_graph, node.key, col));
    } else if(db_graph->col_covgs != NULL) {
      assert(db_node_covg(db_graph, node.key, col) > 0);
    }

    #ifdef CTXVERBOSE
      char bkmerstr[MAX_KMER_SIZE+1];
      binary_kmer_to_str(bkmer, db_graph->kmer_size, bkmerstr);
      status("klen: %zu plen: %zu %zu:%i %s",
             klen, plen, (size_t)node.key, node.orient, bkmerstr);
    #endif

    if(klen == 1) {
      dBNode rnode = db_node_reverse(node);
      Edges backedges = db_node_oriented_edges_in_col(rnode, col, db_graph);
      int outdegree = edges_get_outdegree(backedges, FORWARD);
      if(outdegree <= 1) {
        status("outdegree: %i col: %zu", (int)outdegree, col);
      }
      assert(outdegree > 1);
    }

    n = db_graph_next_nodes(db_graph, bkmer, node.orient,
                            edges, nodes, orients, nucs);

    assert(n > 0);

    // Reduce to nodes in our colour if edges limited
    if(db_graph->num_edge_cols == 1 && db_graph->node_in_cols != NULL) {
      for(i = 0, j = 0; i < n; i++) {
        if(db_node_has_col(db_graph, nodes[i], col)) {
          nodes[j] = nodes[i];
          orients[j] = orients[i];
          nucs[j] = nucs[i];
          j++;
        }
      }
      n = j; // update number of next nodes
      assert(n > 0);
    }

    // If fork check nucleotide
    if(n > 1) {
      Nucleotide expbase = packed_fetch(packed, plen);

      for(i = 0; i < n && nucs[i] != expbase; i++);
      if(i == n) {
        printf("plen: %zu expected: %c\n", plen, dna_nuc_to_char(expbase));
        printf("Got: ");
        for(i = 0; i < n; i++) printf(" %c", dna_nuc_to_char(nucs[i]));
        printf("\n");
      }
      assert(i < n && nucs[i] == expbase);
      node.key = nodes[i];
      node.orient = orients[i];
      plen++;
    }
    else {
      node.key = nodes[0];
      node.orient = orients[0];
    }
  }
}

static void packed_path_check(hkey_t hkey, const uint8_t *packed,
                              size_t ctxcol, size_t ctpcol,
                              const dBGraph *db_graph)
{
  const PathStore *pstore = &db_graph->pdata;
  PathLen len_bases;
  Orientation orient;
  const uint8_t *colset, *seq;

  colset = packedpath_get_colset(packed);
  assert(bitset_get(colset, ctpcol));

  seq = packedpath_seq(packed, pstore->colset_bytes);
  packedpath_get_len_orient(packed, pstore->colset_bytes, &len_bases, &orient);

  dBNode node = {.key = hkey, .orient = orient};

  // Check length
  size_t nbytes = sizeof(PathIndex) + pstore->colset_bytes +
                  sizeof(PathLen) + packedpath_len_nbytes(len_bases);
  assert(packed + nbytes < pstore->end);

  // Check at least one colour is set
  // uint8_t colset_or = 0;
  // for(i = 0; i < pstore->colset_bytes; i++) colset_or |= colset[i];
  // assert(colset_or != 0);

  // print path
  // print_path(node.key, packed+sizeof(PathIndex)+pstore->colset_bytes, pstore);

  graph_path_check_valid(node, ctxcol, seq, len_bases, db_graph);
}

static void kmer_check_paths(hkey_t node, const dBGraph *db_graph,
                             size_t ctxcol, size_t ctpcol,
                             size_t *npaths_ptr, size_t *nkmers_ptr)
{
  const PathStore *pdata = &db_graph->pdata;
  PathIndex index = db_graph->kmer_paths[node];
  uint8_t *packed;
  size_t num_paths = 0;

  while(index != PATH_NULL)
  {
    packed = pdata->store+index;
    packed_path_check(node, packed, ctxcol, ctpcol, db_graph);
    index = packedpath_get_prev(packed);
    num_paths++;
  }

  *npaths_ptr += num_paths;
  *nkmers_ptr += (num_paths > 0);
}

void graph_paths_check_all_paths(const dBGraph *db_graph,
                                 size_t ctxcol, size_t ctpcol)
{
  size_t num_paths = 0, num_kmers = 0;

  HASH_ITERATE(&db_graph->ht, kmer_check_paths, db_graph, ctxcol, ctpcol,
               &num_paths, &num_kmers);

  assert(num_paths == db_graph->pdata.num_of_paths);
  assert(num_kmers == db_graph->pdata.num_kmers_with_paths);
}

void graph_path_check_path(hkey_t node, PathIndex pindex,
                           size_t ctxcol, size_t ctpcol,
                           const dBGraph *db_graph)
{
  uint8_t *packed = db_graph->pdata.store+pindex;
  packed_path_check(node, packed, ctxcol, ctpcol, db_graph);
}
