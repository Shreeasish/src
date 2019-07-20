#include <sys/ioctl.h>
#include <sys/mtio.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>
 

int
main() {
	struct mtget mt;
	//struct stat sb;

  int filedesc = open("testfile.txt", O_RDONLY | O_WRONLY | O_APPEND);

	if (pledge("error", NULL) == -1)
		err(1, "pledge");
  int y = isatty(filedesc);
  int x = ioctl(filedesc, MTIOCGET, &mt);
  atexit
  return 0;
}



