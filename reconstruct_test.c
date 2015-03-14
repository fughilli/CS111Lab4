#include "reconstruct.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <dirent.h>
#include <netdb.h>
#include <assert.h>
#include <pwd.h>
#include <time.h>
#include <limits.h>
#include <stdbool.h>

#include "md5.h"

char testfilename[] = "testfile.txt";

int main(void)
{
    printf("Splitting file: %s\n", split_file(testfilename)?"success":"failure");

    //printf("Reconstructing file: %s\n", reconstruct_file(testfilename)?"success":"failure");

    return 0;
}
