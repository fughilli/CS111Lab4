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

#include <openssl/md5.h>

char* fname_w_ix(const char* prefix, const char* fname, const char* suffix)
{
    char* fnamebuf = (char*)malloc(strlen(fname) + strlen(prefix) + strlen(suffix) + 1);
    strcpy(fnamebuf, prefix);
    strcat(fnamebuf, fname);
    strcat(fnamebuf, suffix);

    return fnamebuf;
}

index_transfer_ret_t write_index_file(const char* fname, const index_t* index)
{
    if(index->header.ih_nlines > MAX_LINES)
        return INDT_FAIL_BAD_LINECT;

    if(index->header.ih_fsize > MAX_FILESIZ)
        return INDT_FAIL_BAD_SIZE;

    int outfd = open(fname, O_WRONLY | O_EXCL | O_CREAT | O_TRUNC, 0644);

    if(outfd == -1)
        return INDT_FAIL_IOERROR;

    // Write the header
    if(write(outfd, &index->header, sizeof(index_header_t)) != sizeof(index_header_t))
    {
        close(outfd);
        return INDT_FAIL_IOERROR;
    }


    // Write the lines
    uint32_t line;
    for(line = 0; line < index->header.ih_nlines; line++)
    {
        if(write(outfd, &index->i_lines[line], sizeof(index_line_t)) != sizeof(index_line_t))
        {
            close(outfd);
            return INDT_FAIL_IOERROR;
        }
    }

    return INDT_SUCCESS;
}

index_transfer_ret_t read_index_file(const char* fname, index_t* index)
{
    int infd = open(fname, O_RDONLY);
    int readamt;

    if(infd == -1)
        return INDT_FAIL_IOERROR;

    // Read the header
    if((readamt = read(infd, &index->header, sizeof(index_header_t))) != sizeof(index_header_t))
    {
        close(infd);
        printf("Fail: %s\n", strerror(errno));
        return INDT_FAIL_IOERROR;
    }

    if(index->header.ih_nlines > MAX_LINES)
        return INDT_FAIL_BAD_LINECT;

    if(index->header.ih_fsize > MAX_FILESIZ)
        return INDT_FAIL_BAD_SIZE;

    index->i_lines = (index_line_t*)malloc(sizeof(index_line_t) * index->header.ih_nlines);

    // Read the lines
    uint32_t line;
    for(line = 0; line < index->header.ih_nlines; line++)
    {
        if(read(infd, &index->i_lines[line], sizeof(index_line_t)) != sizeof(index_line_t))
        {
            close(infd);
            return INDT_FAIL_IOERROR;
        }
    }

    return INDT_SUCCESS;
}

uint32_t minu(uint32_t a, uint32_t b)
{
    if(a < b)
        return a;
    return b;
}

uint32_t maxu(uint32_t a, uint32_t b)
{
    if(a > b)
        return a;
    return b;
}

index_t* alloc_index(uint32_t fpartsiz, uint32_t filesiz)
{
    // If there are bad arguments, throw a fit
    if(fpartsiz < MIN_PARTSIZ || filesiz > MAX_FILESIZ)
    {
        return NULL;
    }

    // Allocate the index struct
    index_t* ret = (index_t*)malloc(sizeof(index_t));

    // Set the header
    ret->header.ih_fsize = filesiz;
    ret->header.ih_nlines = minu(filesiz/fpartsiz, MAX_LINES-1);

    fpartsiz = filesiz/ret->header.ih_nlines;

    // Compute the size of the last line file (0 if no last line)
    uint32_t last_linesiz = filesiz % fpartsiz;

    if(last_linesiz)
    {
        ret->header.ih_nlines++;
    }

    ret->i_lines = (index_line_t*)malloc(sizeof(index_line_t) * ret->header.ih_nlines);

    uint32_t line;
    for(line = 0; line < ret->header.ih_nlines; line++)
    {
        // Clear the filename
        strcpy(ret->i_lines[line].il_fname, "");

        // Set the numbering
        ret->i_lines[line].il_part_no = line;
        ret->i_lines[line].il_prev_part = line-1;
        ret->i_lines[line].il_next_part = line+1;

        // Clear the digest
        memset(ret->i_lines[line].il_md5digest, 0, MD5_DIGEST_LENGTH);

        ret->i_lines[line].il_siz_bytes = fpartsiz;

        if((line+1) == ret->header.ih_nlines)
        {
            ret->i_lines[line].il_next_part = -1;
        }
    }

    if(last_linesiz)
        ret->i_lines[ret->header.ih_nlines-1].il_siz_bytes = last_linesiz;

    return ret;
}

void free_index(index_t* index)
{
    free(index->i_lines);
    free(index);
}

bool md5_file(const char* fname, uint8_t digest[MD5_DIGEST_LENGTH])
{
    uint8_t transferbuf[1024];
    MD5_CTX md5state;
    MD5_Init(&md5state);

    int fd = open(fname, O_RDONLY | O_NONBLOCK);
    if(fd == -1)
        return false;

    while(true)
    {
        int readsiz = read(fd, transferbuf, 1024);

        if(readsiz == 0)
            break;
        if(readsiz == -1)
            return false;

        MD5_Update(&md5state, transferbuf, readsiz);

    }

    MD5_Final((unsigned char*)digest, &md5state);

    close(fd);

    return true;
}

bool
split_file(const char* fname)
{
    int infd = open(fname, O_RDONLY);

    if(infd == -1)
        return false;

    struct stat filestats;
    fstat(infd, &filestats);
    if(filestats.st_size > MAX_FILESIZ)
        return false;

    index_t* index = alloc_index(MIN_PARTSIZ, filestats.st_size);

    md5_file(fname, index->header.ih_md5digest);

    // Allocate a buffer to put the partial files into
    uint8_t *splitbuf = (uint8_t*)malloc(index->i_lines[0].il_siz_bytes);

    uint32_t line;
    for(line = 0; line < index->header.ih_nlines; line++)
    {
        static char numbuf[20];
        sprintf(numbuf, ".part_%05d", line);
        char* temp = fname_w_ix("", fname, numbuf);
        strcpy(index->i_lines[line].il_fname, temp);
        free(temp);

        int outfd = open(index->i_lines[line].il_fname, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);

        read(infd, splitbuf, index->i_lines[line].il_siz_bytes);
        write(outfd, splitbuf, index->i_lines[line].il_siz_bytes);

        close(outfd);

        md5_file(index->i_lines[line].il_fname, index->i_lines[line].il_md5digest);
    }

    close(infd);

    char* outidxname = fname_w_ix("", fname, ".idx");
    write_index_file(outidxname, index);
    free(outidxname);

    return true;
}

bool
verify_digest(uint8_t adigest[MD5_DIGEST_LENGTH], uint8_t bdigest[MD5_DIGEST_LENGTH])
{
    return (memcmp(adigest, bdigest, MD5_DIGEST_LENGTH) == 0);
}

bool
reconstruct_file(const char* fname)
{
    index_t _index;
    index_t* index = &_index;
    uint8_t vdigest[MD5_DIGEST_LENGTH];

    char* inidxname = fname_w_ix("", fname, ".idx");

    if(read_index_file(inidxname, index) != INDT_SUCCESS)
        return false;

    free(inidxname);

    char* outfname = fname_w_ix("rc-", fname, "");

    int rffd = open(outfname, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);

    free(outfname);

    if(rffd == -1)
        return false;

    // Allocate a buffer to put the partial files into
    uint8_t *splitbuf = (uint8_t*)malloc(index->i_lines[0].il_siz_bytes);

    uint32_t line;
    for(line = 0; line < index->header.ih_nlines; line++)
    {
        int infd = open(index->i_lines[line].il_fname, O_RDONLY);

        if(infd == -1)
        {
            // There is a file part missing.
            return false;
        }

        // Compare file sizes

        read(infd, splitbuf, index->i_lines[line].il_siz_bytes);
        write(rffd, splitbuf, index->i_lines[line].il_siz_bytes);

        close(infd);

        md5_file(index->i_lines[line].il_fname, vdigest);
        if(!verify_digest(vdigest, index->i_lines[line].il_md5digest))
        {
            close(rffd);
            return false;
        }
    }

    close(rffd);

    md5_file(fname, vdigest);
    if(!verify_digest(vdigest, index->header.ih_md5digest))
        return false;

    return true;
}

