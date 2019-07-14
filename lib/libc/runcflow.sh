for d in $(find ./ -name '*.c')
do
  # if [ -d "$d" ]; then
  b=`echo ${d:2:-2} | tr / -`-
  c='callgraph-symbol'
  e=`basename ${d:0:-2}`
  echo $e
  cp -t ./cg-symbolentry/ $d
  # cflow -m $e -i _s  --level begin='' --level '0=    |' --level '1=    |' --level end0='' --level end1='' $d > 'cgs/'$b$c
  # fi
done





# arch-amd64-sys-callgraph-no-static
#arch-amd64-net-callgraph-no-static
#arch-callgraph-no-static
#include-callgraph-no-static
#db-callgraph-no-static
#db-man-callgraph-no-static
#arch-amd64-string-callgraph-no-static
# arch-amd64-callgraph-no-static hidden*calllgraph*

