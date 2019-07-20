#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>
#include <unistd.h>

int
main() {
  char * path = ".";
  char * const * paths = &path;
  pledge("stdio", NULL);
  fts_open(paths, FTS_LOGICAL, NULL);
  return 0;
}
