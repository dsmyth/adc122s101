#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

unsigned char Buff[1024*256];

int
main( int argc, char **argv )
/*
  This reads from a device implementing the adc128s052 driver.  It assumes
  the device name is /dev/adc<n> where "n" is passed to this as a command line arg.
  It checks to see that the file offset advances the same amount as data read each
  time, meaning that no samples were lost.
 */
{
    int reads, readstodo, z, c, v, fd;
    ssize_t s_in, s_out, total;
    struct timespec tspec;
    char fname[128];

    if ( argc < 2 ) {
        printf("pass in device name as argument\n");
        return 1;
    }

    readstodo = 0;
    if ( argc > 2 ) {
        readstodo = atoi( argv[2] );
    }

    fname[0] = 0;
    strcat(fname, argv[1]);

    fd = open(fname, O_RDONLY);

    c = 0;
    z = 0;
    total = 0;
    reads = 0;
    while ( (readstodo == 0) || (reads < readstodo) ) {
        s_in = read( fd, Buff, sizeof(Buff) );
        if ( s_in < 0 ) {
            perror("read failed");
            break;
        }
        if ( s_in == 0 ) {
            continue;
        }
        reads++;
        total += s_in;

	// write to stdout
	s_out = write(1, Buff, s_in);

	if (s_out < 0) {
		perror("write failed: ");
	}
    }  // end of while

    //printf("total %d\n", t);
    v = close(fd);
    if (v < 0) {
        perror("close failed: ");
    }
    return 0;
}
