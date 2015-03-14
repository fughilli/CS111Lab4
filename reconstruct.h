#ifndef _RECONSTRUCT_H_
#define _RECONSTRUCT_H_

#include <stdbool.h>
#include <openssl/md5.h>
#include <stdint.h>

#define WORKING_SUBDIR "./.tmp/"

#define MAX_FNAMESIZ 256

#define MAX_FILESIZ 10000000
#define MAX_LINES 20
#define MIN_PARTSIZ 50

typedef struct
{
    int32_t il_part_no;

    char il_fname[MAX_FNAMESIZ];

    uint32_t il_siz_bytes;

    uint8_t il_md5digest[MD5_DIGEST_LENGTH];

    int32_t il_next_part;
    int32_t il_prev_part;
} index_line_t;

typedef struct
{
    uint32_t ih_fsize;
    uint8_t ih_md5digest[MD5_DIGEST_LENGTH];
    uint32_t ih_nlines;
} index_header_t;

typedef struct
{
    // Header
    index_header_t header;

    index_line_t *i_lines;
} index_t;

typedef enum
{
    INDT_SUCCESS,
    INDT_FAIL_BAD_SIZE,
    INDT_FAIL_BAD_LINECT,
    INDT_FAIL_IOERROR
} index_transfer_ret_t;

index_t* alloc_index(uint32_t fpartsiz, uint32_t filesiz);

void free_index(index_t* index);

index_transfer_ret_t read_index_file(const char* fname, index_t* index);
index_transfer_ret_t write_index_file(const char* fname, const index_t* index);

char*
fname_w_ix(const char* prefix, const char* fname, const char* suffix);

bool
split_file(const char* fname);

bool
reconstruct_file(const char* fname);

#endif // _RECONSTRUCT_H_

