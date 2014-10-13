#!/bin/bash

set -euo pipefail

CTXDIR=../..
CTXK=$CTXDIR/bin/ctx
READSIM=$CTXDIR/libs/readsim/readsim
ALLREADS=$CTXDIR/libs/seq_file/scripts/perfect_covg.sh
STRCHK=$CTXDIR/libs/bioinf-perl/sim_mutations/sim_substrings.pl
CONTIG_STATS=$CTXDIR/libs/bioinf-perl/fastn_scripts/contig_stats.pl
# DNACAT=$CTXDIR/libs/seq_file/bin/dnacat

REF=$CTXDIR/results/data/chr22/uniq_flanks/chr22.1Mbp.uniq.fa
ERR_PROFILE=$CTXDIR/results/data/PhiX/PhiX.1.fq.gz
READLEN=100
DEPTH=100

# How many contigs to pull out to find median walk distance
NSEED_WALK=100

run () {
  cmd="$@"
  # Print to STDERR
  echo $cmd 1>&2
  $cmd
}

getctx () {
  k=$1
  echo "$CTXK"$[ ($k+31)/32*32-1 ];
}

kmers=$(echo 15 21 31 41 51 63 75 99)
nkmers=$(echo $kmers | tr ' ' '\n' | awk 'END{print NR}')

MEM=5G

# create directories
for k in $kmers; do [ ! -d k$k ] && run mkdir -p k$k; done
mkdir -p logs reads

# Generate reads
[ ! -f reads/perf.fa.gz  ]    && run $ALLREADS $READLEN $REF | gzip -c > reads/perf.fa.gz
[ ! -f reads/stoch.fa.gz ]    && run $READSIM -l $READLEN -r $REF -d $DEPTH -s reads/stoch
[ ! -f reads/stocherr.fa.gz ] && run $READSIM -l $READLEN -r $REF -d $DEPTH -e 0.005 -s reads/stocherr

# Cortex build k=$(K)
echo == Building cortex graphs ==

for k in $kmers; do
  [ ! -f k$k/perf.ctx ]         && run `getctx $k` build -m $MEM -k $k --sample chr22_17M_18M --seq reads/perf.fa.gz k$k/perf.ctx
  [ ! -f k$k/stoch.ctx ]        && run `getctx $k` build -m $MEM -k $k --sample chr22_17M_18M --seq reads/stoch.fa.gz k$k/stoch.ctx
  [ ! -f k$k/stocherr.raw.ctx ] && run `getctx $k` build -m $MEM -k $k --sample chr22_17M_18M --seq reads/stocherr.fa.gz k$k/stocherr.raw.ctx
  [ ! -f k$k/stocherr.ctx ]     && run `getctx $k` clean -m $MEM --covg-before k$k/stocherr.raw.covg.csv --out k$k/stocherr.ctx k$k/stocherr.raw.ctx
done

echo == Read threading ==

for k in $kmers; do
  for p in perf stoch stocherr; do
    [ ! -f k$k/$p.se.ctp.gz ] && run `getctx $k` thread -m $MEM --seq reads/$p.fa.gz --contig-hist k$k/$p.se.rlenhist.csv --out k$k/$p.se.ctp.gz k$k/$p.ctx
  done
done

echo == Assembling contigs ==

for k in $kmers; do
  for p in perf stoch stocherr; do
    [ ! -f k$k/$p.plain.contigs.fa       ] && run `getctx $k` contigs -m $MEM -o k$k/$p.plain.contigs.fa k$k/$p.ctx
    [ ! -f k$k/$p.links.contigs.fa       ] && run `getctx $k` contigs -m $MEM -o k$k/$p.links.contigs.fa -p k$k/$p.se.ctp.gz k$k/$p.ctx
    [ ! -f k$k/$p.plain.contigs.rmdup.fa ] && ( run `getctx $k` rmsubstr -k $k -m $MEM -q k$k/$p.plain.contigs.fa ) > k$k/$p.plain.contigs.rmdup.fa
    [ ! -f k$k/$p.links.contigs.rmdup.fa ] && ( run `getctx $k` rmsubstr -k $k -m $MEM -q k$k/$p.links.contigs.fa ) > k$k/$p.links.contigs.rmdup.fa
  done
done

echo == Median walk distance ==

med_walk() {
  k=$1; p=$2; pathargs=$3;
  ctx=$(getctx $k)
  dist=$($ctx contigs --reseed --ncontigs $NSEED_WALK $pathargs k$k/$p.ctx 2>&1 | \
         grep -ioE 'Lengths:.*median: [0-9,]*' | grep -oE '[0-9,]+$' | tr -d ',')
  printf "med_walk,$dist\n"
}

for k in $kmers; do
  for p in perf stoch stocherr; do
    [ ! -f k$k/$p.plain.medwalk.txt ] && med_walk $k $p ''                    > k$k/$p.plain.medwalk.txt
    [ ! -f k$k/$p.links.medwalk.txt ] && med_walk $k $p "-p k$k/$p.se.ctp.gz" > k$k/$p.links.medwalk.txt
  done
done

# Contig stats
echo == Contig stats ==

for k in $kmers; do
  for p in perf stoch stocherr; do
    for annot in plain links; do
      [ ! -f k$k/$p.$annot.contigs.rmdup.csv ] && \
        run $CONTIG_STATS --print-csv k$k/$p.$annot.contigs.rmdup.fa | \
        cat - k$k/$p.$annot.medwalk.txt > k$k/$p.$annot.contigs.rmdup.csv
    done
  done
done

# Combine CSV files to summarise statistics
echo == Merging CSV files ==

colidx=$(echo $(eval echo '{1,$[{1..'$nkmers'}*2]}') | tr ' ' ',');

for p in perf stoch stocherr; do
  for annot in plain links; do
    [ ! -f $p.$annot.join.csv ] && \
      (printf "metric,%s\n" $(echo $kmers | sed 's/ /,k/g');
       printf "kmer,%s\n" $(echo $kmers | tr ' ' ',');
       paste -d, k*/$p.$annot.contigs.rmdup.csv | \
       cut -d, -f $colidx - | tail -n +2) > $p.$annot.join.csv
  done
done

# Stats
echo == Checking contig matches ==

for k in $kmers; do
  for p in perf stoch stocherr; do
    for annot in plain links; do
      run $STRCHK $k 0.1 k$k/$p.$annot.contigs.rmdup.fa ../../results/data/chr22/chr22_17M_18M.fa
    done
  done
done

# Now make plots with:
run mkdir -p plots
echo Plot with:
echo "  " R --vanilla -f plot-results.R --args {perf,stoch,stocherr}.{links,plain}.join.csv
