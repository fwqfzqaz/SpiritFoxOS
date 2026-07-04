/*
 * gunzip.c - Lightweight gzip/deflate decompressor for SpiritFoxOS kernel
 *
 * Implements RFC 1951 (DEFLATE) and RFC 1952 (GZIP) decompression
 * for use by the package manager to extract data.tar.gz from .deb packages.
 *
 * No standard library dependencies - uses only kmalloc/kfree, memcpy, etc.
 */

#include "gunzip.h"
#include "kmalloc.h"
#include "string.h"
#include "vga.h"
#include "vfs.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define GUNZIP_OK            0
#define GUNZIP_ERR_MAGIC    -1
#define GUNZIP_ERR_METHOD   -2
#define GUNZIP_ERR_FLAGS    -3
#define GUNZIP_ERR_DATA     -4
#define GUNZIP_ERR_CRC      -5
#define GUNZIP_ERR_SIZE     -6
#define GUNZIP_ERR_MEMORY   -7
#define GUNZIP_ERR_OVERFLOW -8
#define GUNZIP_ERR_HUFFMAN  -9
#define GUNZIP_ERR_BLOCK   -10

#define MAX_DECOMPRESSED_SIZE  (256UL * 1024 * 1024)  /* 256 MB */
#define GZIP_MAGIC_0           0x1f
#define GZIP_MAGIC_1           0x8b
#define GZIP_METHOD_DEFLATE    8

#define GZIP_FLAG_FTEXT    0x01
#define GZIP_FLAG_FHCRC    0x02
#define GZIP_FLAG_FEXTRA   0x04
#define GZIP_FLAG_FNAME    0x08
#define GZIP_FLAG_FCOMMENT 0x10

/* Max bits for a Huffman code */
#define MAX_BITS       15
/* Number of length codes */
#define NUM_LIT_CODES  286
/* Number of distance codes */
#define NUM_DIST_CODES 30
/* Number of code length codes */
#define NUM_CL_CODES   19

/* ========================================================================
 * CRC32 table and computation
 * ======================================================================== */

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    crc = crc ^ 0xffffffff;
    while (len--)
        crc = crc32_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffff;
}

/* ========================================================================
 * Bitstream reader
 * ======================================================================== */

typedef struct {
    const uint8_t *data;
    size_t         size;    /* total bytes in source */
    size_t         bytepos; /* current byte position */
    unsigned int   bitbuf;  /* bits we've loaded but not yet consumed */
    int            bitcnt;  /* number of bits in bitbuf */
} bitstream_t;

static void bs_init(bitstream_t *bs, const uint8_t *data, size_t size)
{
    bs->data   = data;
    bs->size   = size;
    bs->bytepos = 0;
    bs->bitbuf = 0;
    bs->bitcnt = 0;
}

/* Load more bits into the bitstream buffer */
static int bs_load(bitstream_t *bs)
{
    while (bs->bitcnt <= 24 && bs->bytepos < bs->size) {
        bs->bitbuf |= (unsigned int)bs->data[bs->bytepos] << bs->bitcnt;
        bs->bytepos++;
        bs->bitcnt += 8;
    }
    return (bs->bitcnt > 0) ? 0 : -1;
}

/* Peek at n bits without consuming them (n must be <= 24) */
static int bs_peek(bitstream_t *bs, int n, unsigned int *val)
{
    if (bs->bitcnt < n) {
        if (bs_load(bs) < 0)
            return -1;
    }
    if (bs->bitcnt < n)
        return -1;
    *val = bs->bitbuf & ((1u << n) - 1);
    return 0;
}

/* Consume n bits */
static void bs_drop(bitstream_t *bs, int n)
{
    bs->bitbuf >>= n;
    bs->bitcnt -= n;
}

/* Read n bits (peek + drop) */
static int bs_read(bitstream_t *bs, int n, unsigned int *val)
{
    if (bs_peek(bs, n, val) < 0)
        return -1;
    bs_drop(bs, n);
    return 0;
}

/* Align to next byte boundary */
static void bs_align(bitstream_t *bs)
{
    int discard = bs->bitcnt & 7;
    if (discard) {
        bs->bitbuf >>= discard;
        bs->bitcnt -= discard;
    }
}

/* Read n bytes from the bitstream (after byte-aligning), big-endian */
static int bs_read_bytes(bitstream_t *bs, int n, unsigned int *val)
{
    bs_align(bs);
    /* discard any remaining partial bits */
    bs->bitbuf = 0;
    bs->bitcnt = 0;

    *val = 0;
    for (int i = 0; i < n; i++) {
        if (bs->bytepos >= bs->size)
            return -1;
        *val = (*val << 8) | bs->data[bs->bytepos++];
    }
    return 0;
}

/* ========================================================================
 * Huffman decoding
 * ======================================================================== */

typedef struct {
    uint16_t counts[MAX_BITS + 1]; /* number of codes of each length */
    uint16_t symbols[288];         /* symbols sorted by code */
} huffman_t;

/*
 * Build a Huffman decoding table from an array of code lengths.
 * code_lengths[i] = bit-length of code for symbol i (0 = not present).
 * num_symbols = total number of symbols.
 * max_sym = max symbol value that can appear in the symbols array.
 */
static int huffman_build(huffman_t *h, const uint8_t *code_lengths,
                         int num_symbols)
{
    int i;
    unsigned int next_code[MAX_BITS + 1];

    memset(h, 0, sizeof(*h));

    /* Count code lengths */
    for (i = 0; i < num_symbols; i++) {
        if (code_lengths[i] > MAX_BITS) {
            printf("[gunzip] invalid code length %d for symbol %d\n",
                   code_lengths[i], i);
            return GUNZIP_ERR_HUFFMAN;
        }
        h->counts[code_lengths[i]]++;
    }

    /* Offset of first code of each length */
    h->counts[0] = 0;
    next_code[0] = 0;
    for (i = 1; i <= MAX_BITS; i++) {
        next_code[i] = (next_code[i - 1] + h->counts[i - 1]) << 1;
        /* Overflow check: codes of length i must fit in i bits */
        if (next_code[i] + h->counts[i] > (1u << i)) {
            printf("[gunzip] huffman code overflow at length %d\n", i);
            return GUNZIP_ERR_HUFFMAN;
        }
    }

    /* Build symbol table sorted by code */
    /* We use counts as an index into symbols[] */
    uint16_t offsets[MAX_BITS + 1];
    offsets[0] = 0;
    for (i = 1; i <= MAX_BITS; i++)
        offsets[i] = offsets[i - 1] + h->counts[i - 1];

    for (i = 0; i < num_symbols; i++) {
        if (code_lengths[i]) {
            h->symbols[offsets[code_lengths[i]]++] = (uint16_t)i;
        }
    }

    return GUNZIP_OK;
}

/*
 * Decode one symbol from the bitstream using the Huffman table.
 */
static int huffman_decode(bitstream_t *bs, const huffman_t *h,
                          unsigned int *sym)
{
    int code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= MAX_BITS; len++) {
        unsigned int bit;
        if (bs_read(bs, 1, &bit) < 0)
            return GUNZIP_ERR_DATA;
        code = (code << 1) | (int)bit;
        int count = h->counts[len];
        if (code - count < first) {
            *sym = h->symbols[index + (code - first)];
            return GUNZIP_OK;
        }
        index += count;
        first = (first + count) << 1;
    }

    return GUNZIP_ERR_HUFFMAN;
}

/* ========================================================================
 * RFC 1951 DEFLATE tables
 * ======================================================================== */

/*
 * Length code base values and extra bits (codes 257..285).
 * Index 0 = code 257, index 28 = code 285.
 */
static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10,       /* 257-264 */
    11, 13, 15, 17,                  /* 265-268 */
    19, 23, 27, 31,                  /* 269-272 */
    35, 43, 51, 59,                  /* 273-276 */
    67, 83, 99, 115,                 /* 277-280 */
    131, 163, 195, 227,              /* 281-284 */
    258                               /* 285 */
};

static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1,
    2, 2, 2, 2,
    3, 3, 3, 3,
    4, 4, 4, 4,
    5, 5, 5, 5,
    0
};

/*
 * Distance code base values and extra bits (codes 0..29).
 */
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13,
    17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073,
    4097, 6145, 8193, 12289, 16385, 24577
};

static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2,
    3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10,
    11, 11, 12, 12, 13, 13
};

/*
 * Order of code length code lengths in the dynamic block header.
 */
static const uint8_t cl_order[NUM_CL_CODES] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* ========================================================================
 * Fixed Huffman tables (block type 1)
 * ======================================================================== */

static huffman_t fixed_lit;
static huffman_t fixed_dist;
static int fixed_tables_built = 0;

static void build_fixed_tables(void)
{
    uint8_t lengths[288];
    int i;

    /* Literal/length: 0-143 = 8 bits, 144-255 = 9 bits,
     * 256-279 = 7 bits, 280-287 = 8 bits */
    for (i = 0; i <= 143; i++)   lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huffman_build(&fixed_lit, lengths, 288);

    /* Distance: all 32 codes are 5 bits */
    for (i = 0; i < 32; i++) lengths[i] = 5;
    huffman_build(&fixed_dist, lengths, 32);

    fixed_tables_built = 1;
}

/* ========================================================================
 * Inflate output buffer
 * ======================================================================== */

typedef struct {
    uint8_t *data;
    size_t   cap;     /* allocated capacity */
    size_t   pos;     /* current write position */
} output_t;

static int out_init(output_t *o, size_t cap)
{
    o->data = (uint8_t *)kmalloc(cap);
    if (!o->data) {
        printf("[gunzip] out of memory allocating %u byte output buffer\n",
               (unsigned)cap);
        return GUNZIP_ERR_MEMORY;
    }
    o->cap = cap;
    o->pos = 0;
    return GUNZIP_OK;
}

static void out_free(output_t *o)
{
    if (o->data) {
        kfree(o->data);
        o->data = NULL;
    }
    o->cap = 0;
    o->pos = 0;
}

static int out_ensure(output_t *o, size_t additional)
{
    if (o->pos + additional <= o->cap)
        return GUNZIP_OK;

    /* Grow to at least pos + additional, double if larger */
    size_t new_cap = o->cap * 2;
    if (new_cap < o->pos + additional)
        new_cap = o->pos + additional;

    if (new_cap > MAX_DECOMPRESSED_SIZE) {
        printf("[gunzip] decompressed data exceeds %u MB limit\n",
               (unsigned)(MAX_DECOMPRESSED_SIZE / (1024 * 1024)));
        return GUNZIP_ERR_OVERFLOW;
    }

    uint8_t *new_data = (uint8_t *)kmalloc(new_cap);
    if (!new_data) {
        printf("[gunzip] out of memory growing output to %u bytes\n",
               (unsigned)new_cap);
        return GUNZIP_ERR_MEMORY;
    }
    memcpy(new_data, o->data, o->pos);
    kfree(o->data);
    o->data = new_data;
    o->cap = new_cap;
    return GUNZIP_OK;
}

static int out_write_byte(output_t *o, uint8_t b)
{
    if (o->pos >= o->cap) {
        int rc = out_ensure(o, 1);
        if (rc < 0) return rc;
    }
    o->data[o->pos++] = b;
    return GUNZIP_OK;
}

static int out_copy_match(output_t *o, size_t dist, size_t len)
{
    if (dist == 0 || dist > o->pos) {
        printf("[gunzip] invalid back-reference: dist=%u pos=%u\n",
               (unsigned)dist, (unsigned)o->pos);
        return GUNZIP_ERR_DATA;
    }

    int rc = out_ensure(o, len);
    if (rc < 0) return rc;

    /* Must copy byte-by-byte because source and dest may overlap */
    size_t src = o->pos - dist;
    for (size_t i = 0; i < len; i++) {
        o->data[o->pos++] = o->data[src + i];
    }
    return GUNZIP_OK;
}

/* ========================================================================
 * DEFLATE inflate implementation
 * ======================================================================== */

/*
 * Decode a dynamic Huffman block (type 2).
 * Reads the code length tables and builds literal+length and distance
 * Huffman trees, then inflates the data.
 */
static int inflate_dynamic_block(bitstream_t *bs, output_t *out)
{
    unsigned int hlit, hdist, hclen;
    uint8_t cl_lengths[NUM_CL_CODES];
    uint8_t lit_lengths[NUM_LIT_CODES + NUM_DIST_CODES];
    int i;
    int num_lit, num_dist;

    /* Read table counts */
    if (bs_read(bs, 5, &hlit) < 0)  return GUNZIP_ERR_DATA;
    if (bs_read(bs, 5, &hdist) < 0) return GUNZIP_ERR_DATA;
    if (bs_read(bs, 4, &hclen) < 0) return GUNZIP_ERR_DATA;

    hlit  += 257;   /* number of literal/length codes */
    hdist += 1;     /* number of distance codes */
    hclen += 4;     /* number of code length codes */

    num_lit  = (int)hlit;
    num_dist = (int)hdist;

    if (num_lit > NUM_LIT_CODES || num_dist > NUM_DIST_CODES) {
        printf("[gunzip] dynamic block: hlit=%u hdist=%u out of range\n",
               hlit, hdist);
        return GUNZIP_ERR_DATA;
    }

    /* Read code length code lengths */
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (i = 0; i < (int)hclen; i++) {
        unsigned int val;
        if (bs_read(bs, 3, &val) < 0)
            return GUNZIP_ERR_DATA;
        cl_lengths[cl_order[i]] = (uint8_t)val;
    }

    /* Build code length Huffman table */
    huffman_t cl_table;
    int rc = huffman_build(&cl_table, cl_lengths, NUM_CL_CODES);
    if (rc < 0) return rc;

    /* Decode literal/length + distance code lengths */
    int total = num_lit + num_dist;
    memset(lit_lengths, 0, sizeof(lit_lengths));

    i = 0;
    while (i < total) {
        unsigned int sym;
        rc = huffman_decode(bs, &cl_table, &sym);
        if (rc < 0) {
            printf("[gunzip] failed to decode code length symbol\n");
            return rc;
        }

        if (sym < 16) {
            /* Literal code length */
            lit_lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            /* Repeat previous length 3-6 times */
            unsigned int rep;
            if (bs_read(bs, 2, &rep) < 0) return GUNZIP_ERR_DATA;
            rep += 3;
            if (i == 0 || i + rep > total) {
                printf("[gunzip] code 16 repeat at i=%d rep=%u total=%d\n",
                       i, rep, total);
                return GUNZIP_ERR_DATA;
            }
            uint8_t prev = lit_lengths[i - 1];
            while (rep--)
                lit_lengths[i++] = prev;
        } else if (sym == 17) {
            /* Repeat zero 3-10 times */
            unsigned int rep;
            if (bs_read(bs, 3, &rep) < 0) return GUNZIP_ERR_DATA;
            rep += 3;
            if (i + rep > total) return GUNZIP_ERR_DATA;
            while (rep--)
                lit_lengths[i++] = 0;
        } else if (sym == 18) {
            /* Repeat zero 11-138 times */
            unsigned int rep;
            if (bs_read(bs, 7, &rep) < 0) return GUNZIP_ERR_DATA;
            rep += 11;
            if (i + rep > total) return GUNZIP_ERR_DATA;
            while (rep--)
                lit_lengths[i++] = 0;
        } else {
            printf("[gunzip] invalid code length symbol %u\n", sym);
            return GUNZIP_ERR_DATA;
        }
    }

    /* Build literal/length and distance Huffman tables */
    huffman_t lit_table, dist_table;

    rc = huffman_build(&lit_table, lit_lengths, num_lit);
    if (rc < 0) return rc;

    /* Distance table: if all zero lengths, distance codes are not used */
    int has_dist = 0;
    for (int j = 0; j < num_dist; j++) {
        if (lit_lengths[num_lit + j] != 0) {
            has_dist = 1;
            break;
        }
    }

    if (has_dist) {
        rc = huffman_build(&dist_table, &lit_lengths[num_lit], num_dist);
        if (rc < 0) return rc;
    } else {
        memset(&dist_table, 0, sizeof(dist_table));
    }

    /* Inflate using the two tables */
    for (;;) {
        unsigned int sym;
        rc = huffman_decode(bs, &lit_table, &sym);
        if (rc < 0) {
            printf("[gunzip] failed to decode literal symbol\n");
            return rc;
        }

        if (sym < 256) {
            /* Literal byte */
            rc = out_write_byte(out, (uint8_t)sym);
            if (rc < 0) return rc;
        } else if (sym == 256) {
            /* End of block */
            break;
        } else {
            /* Length/distance pair */
            unsigned int len_idx = sym - 257;
            if (len_idx >= 29) {
                printf("[gunzip] invalid length code %u\n", sym);
                return GUNZIP_ERR_DATA;
            }

            size_t length = len_base[len_idx];
            if (len_extra[len_idx] > 0) {
                unsigned int extra;
                if (bs_read(bs, len_extra[len_idx], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                length += extra;
            }

            /* Decode distance */
            unsigned int dist_sym;
            if (!has_dist) {
                printf("[gunzip] distance code with no distance table\n");
                return GUNZIP_ERR_DATA;
            }
            rc = huffman_decode(bs, &dist_table, &dist_sym);
            if (rc < 0) {
                printf("[gunzip] failed to decode distance symbol\n");
                return rc;
            }
            if (dist_sym >= 30) {
                printf("[gunzip] invalid distance code %u\n", dist_sym);
                return GUNZIP_ERR_DATA;
            }

            size_t distance = dist_base[dist_sym];
            if (dist_extra[dist_sym] > 0) {
                unsigned int extra;
                if (bs_read(bs, dist_extra[dist_sym], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                distance += extra;
            }

            rc = out_copy_match(out, distance, length);
            if (rc < 0) return rc;
        }
    }

    return GUNZIP_OK;
}

/*
 * Inflate a fixed Huffman block (type 1).
 */
static int inflate_fixed_block(bitstream_t *bs, output_t *out)
{
    if (!fixed_tables_built)
        build_fixed_tables();

    for (;;) {
        unsigned int sym;
        int rc = huffman_decode(bs, &fixed_lit, &sym);
        if (rc < 0) {
            printf("[gunzip] fixed block: decode error\n");
            return rc;
        }

        if (sym < 256) {
            rc = out_write_byte(out, (uint8_t)sym);
            if (rc < 0) return rc;
        } else if (sym == 256) {
            break;
        } else {
            /* Length/distance pair */
            unsigned int len_idx = sym - 257;
            if (len_idx >= 29) {
                printf("[gunzip] fixed block: invalid length code %u\n", sym);
                return GUNZIP_ERR_DATA;
            }

            size_t length = len_base[len_idx];
            if (len_extra[len_idx] > 0) {
                unsigned int extra;
                if (bs_read(bs, len_extra[len_idx], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                length += extra;
            }

            /* Decode distance */
            unsigned int dist_sym;
            rc = huffman_decode(bs, &fixed_dist, &dist_sym);
            if (rc < 0) {
                printf("[gunzip] fixed block: distance decode error\n");
                return rc;
            }
            if (dist_sym >= 30) {
                printf("[gunzip] fixed block: invalid distance code %u\n",
                       dist_sym);
                return GUNZIP_ERR_DATA;
            }

            size_t distance = dist_base[dist_sym];
            if (dist_extra[dist_sym] > 0) {
                unsigned int extra;
                if (bs_read(bs, dist_extra[dist_sym], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                distance += extra;
            }

            rc = out_copy_match(out, distance, length);
            if (rc < 0) return rc;
        }
    }

    return GUNZIP_OK;
}

/*
 * Inflate a stored (uncompressed) block (type 0).
 */
static int inflate_stored_block(bitstream_t *bs, output_t *out)
{
    unsigned int len, nlen;

    /* Align to byte boundary */
    bs_align(bs);
    bs->bitbuf = 0;
    bs->bitcnt = 0;

    /* Read LEN and NLEN */
    if (bs->bytepos + 4 > bs->size) {
        printf("[gunzip] stored block: truncated header\n");
        return GUNZIP_ERR_DATA;
    }

    len  = (unsigned int)bs->data[bs->bytepos]
         | ((unsigned int)bs->data[bs->bytepos + 1] << 8);
    nlen = (unsigned int)bs->data[bs->bytepos + 2]
         | ((unsigned int)bs->data[bs->bytepos + 3] << 8);
    bs->bytepos += 4;

    if ((len ^ nlen) != 0xffff) {
        printf("[gunzip] stored block: len/nlen mismatch (len=%u nlen=%u)\n",
               len, nlen);
        return GUNZIP_ERR_DATA;
    }

    if (bs->bytepos + len > bs->size) {
        printf("[gunzip] stored block: data truncated (need %u, have %u)\n",
               len, (unsigned)(bs->size - bs->bytepos));
        return GUNZIP_ERR_DATA;
    }

    /* Ensure output has space */
    int rc = out_ensure(out, len);
    if (rc < 0) return rc;

    /* Copy raw data */
    memcpy(out->data + out->pos, bs->data + bs->bytepos, len);
    out->pos += len;
    bs->bytepos += len;

    return GUNZIP_OK;
}

/*
 * Main inflate loop: process deflate blocks.
 * `src` points to the raw deflate stream (after gzip header if applicable).
 */
static int inflate_stream(bitstream_t *bs, output_t *out)
{
    int bfinal;

    do {
        unsigned int final_bit, type_bits;

        if (bs_read(bs, 1, &final_bit) < 0) {
            printf("[gunzip] failed to read BFINAL\n");
            return GUNZIP_ERR_DATA;
        }
        if (bs_read(bs, 2, &type_bits) < 0) {
            printf("[gunzip] failed to read BTYPE\n");
            return GUNZIP_ERR_DATA;
        }

        bfinal = (int)final_bit;
        int rc;

        switch (type_bits) {
        case 0:
            rc = inflate_stored_block(bs, out);
            break;
        case 1:
            rc = inflate_fixed_block(bs, out);
            break;
        case 2:
            rc = inflate_dynamic_block(bs, out);
            break;
        default:
            printf("[gunzip] reserved block type 3\n");
            return GUNZIP_ERR_BLOCK;
        }

        if (rc < 0)
            return rc;

    } while (!bfinal);

    return GUNZIP_OK;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/*
 * zlib_inflate - Raw DEFLATE decompression
 *
 * @src:      pointer to raw deflate-compressed data
 * @src_size: size of compressed data in bytes
 * @dst:      pre-allocated output buffer
 * @dst_cap:  capacity of the output buffer
 * @dst_size: on success, receives the number of decompressed bytes
 *
 * Returns GUNZIP_OK (0) on success, negative error code on failure.
 */
int zlib_inflate(const void *src, size_t src_size, void *dst,
                 size_t dst_cap, size_t *dst_size)
{
    bitstream_t bs;
    output_t out;
    int rc;

    if (!src || !dst || !dst_size)
        return GUNZIP_ERR_DATA;

    if (src_size == 0) {
        *dst_size = 0;
        return GUNZIP_OK;
    }

    bs_init(&bs, (const uint8_t *)src, src_size);

    /* Use the caller-provided buffer directly */
    out.data = (uint8_t *)dst;
    out.cap  = dst_cap;
    out.pos  = 0;

    rc = inflate_stream(&bs, &out);
    if (rc < 0) {
        *dst_size = 0;
        return rc;
    }

    *dst_size = out.pos;
    return GUNZIP_OK;
}

/*
 * gunzip - Decompress gzip data (RFC 1952)
 *
 * @src:      pointer to gzip-compressed data
 * @src_size: size of compressed data in bytes
 * @dst:      on success, receives a kmalloc'd buffer with decompressed data
 * @dst_size: on success, receives the size of the decompressed data
 *
 * Returns GUNZIP_OK (0) on success, negative error code on failure.
 * Caller must kfree(*dst) when done.
 */
int gunzip(const void *src, size_t src_size, void **dst, size_t *dst_size)
{
    const uint8_t *data;
    size_t pos;
    bitstream_t bs;
    output_t out;
    int rc;

    if (!src || !dst || !dst_size)
        return GUNZIP_ERR_DATA;

    *dst = NULL;
    *dst_size = 0;

    data = (const uint8_t *)src;

    if (src_size < 10) {
        printf("[gunzip] data too short for gzip header\n");
        return GUNZIP_ERR_MAGIC;
    }

    /* ---- Parse gzip header (RFC 1952) ---- */

    /* Magic bytes */
    if (data[0] != GZIP_MAGIC_0 || data[1] != GZIP_MAGIC_1) {
        printf("[gunzip] bad gzip magic: 0x%02x 0x%02x\n", data[0], data[1]);
        return GUNZIP_ERR_MAGIC;
    }

    /* Compression method */
    if (data[2] != GZIP_METHOD_DEFLATE) {
        printf("[gunzip] unsupported compression method %u\n", data[2]);
        return GUNZIP_ERR_METHOD;
    }

    uint8_t flags = data[3];

    /* Reserved bits must be zero */
    if (flags & 0xe0) {
        printf("[gunzip] reserved flag bits set: 0x%02x\n", flags);
        return GUNZIP_ERR_FLAGS;
    }

    /* Skip: MTIME(4), XFL(1), OS(1) = 6 bytes */
    pos = 10;

    /* FEXTRA */
    if (flags & GZIP_FLAG_FEXTRA) {
        if (pos + 2 > src_size) return GUNZIP_ERR_DATA;
        uint16_t xlen = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 2;
        if (pos + xlen > src_size) return GUNZIP_ERR_DATA;
        pos += xlen;
    }

    /* FNAME */
    if (flags & GZIP_FLAG_FNAME) {
        while (pos < src_size && data[pos] != '\0')
            pos++;
        if (pos >= src_size) return GUNZIP_ERR_DATA;
        pos++; /* skip the null terminator */
    }

    /* FCOMMENT */
    if (flags & GZIP_FLAG_FCOMMENT) {
        while (pos < src_size && data[pos] != '\0')
            pos++;
        if (pos >= src_size) return GUNZIP_ERR_DATA;
        pos++;
    }

    /* FHCRC */
    if (flags & GZIP_FLAG_FHCRC) {
        if (pos + 2 > src_size) return GUNZIP_ERR_DATA;
        pos += 2;  /* skip header CRC16 */
    }

    if (pos >= src_size) {
        printf("[gunzip] no compressed data after header\n");
        return GUNZIP_ERR_DATA;
    }

    /* ---- Find the trailer (CRC32 + ISIZE) at the end ---- */
    if (src_size < pos + 8) {
        printf("[gunzip] no room for gzip trailer\n");
        return GUNZIP_ERR_DATA;
    }

    /* The trailer is the last 8 bytes of the gzip stream */
    size_t trailer_off = src_size - 8;
    uint32_t expected_crc = (uint32_t)data[trailer_off]
                          | ((uint32_t)data[trailer_off + 1] << 8)
                          | ((uint32_t)data[trailer_off + 2] << 16)
                          | ((uint32_t)data[trailer_off + 3] << 24);
    uint32_t expected_size = (uint32_t)data[trailer_off + 4]
                           | ((uint32_t)data[trailer_off + 5] << 8)
                           | ((uint32_t)data[trailer_off + 6] << 16)
                           | ((uint32_t)data[trailer_off + 7] << 24);

    /* ---- Inflate ---- */

    /* Start with a reasonable initial output size */
    size_t init_cap = src_size * 4;
    if (init_cap < 4096) init_cap = 4096;
    if (init_cap > MAX_DECOMPRESSED_SIZE) init_cap = MAX_DECOMPRESSED_SIZE;

    rc = out_init(&out, init_cap);
    if (rc < 0) return rc;

    bs_init(&bs, data + pos, trailer_off - pos);

    rc = inflate_stream(&bs, &out);
    if (rc < 0) {
        out_free(&out);
        return rc;
    }

    /* ---- Verify CRC32 ---- */
    uint32_t actual_crc = crc32_update(0, out.data, out.pos);
    if (actual_crc != expected_crc) {
        printf("[gunzip] CRC32 mismatch: expected 0x%08x got 0x%08x\n",
               expected_crc, actual_crc);
        out_free(&out);
        return GUNZIP_ERR_CRC;
    }

    /* ---- Verify ISIZE (original size mod 2^32) ---- */
    if ((uint32_t)(out.pos & 0xffffffff) != expected_size) {
        printf("[gunzip] ISIZE mismatch: expected %u got %u\n",
               expected_size, (unsigned)(out.pos & 0xffffffff));
        out_free(&out);
        return GUNZIP_ERR_SIZE;
    }

    *dst = out.data;
    *dst_size = out.pos;

    return GUNZIP_OK;
}

/* ========================================================================
 * Streaming gzip decompression via VFS
 *
 * Reads compressed data incrementally from a VFS file descriptor,
 * decompresses using a fixed output buffer with sliding window,
 * and calls a user callback for each chunk of decompressed data.
 * Maximum memory usage: ~4.25MB (4MB output + 256KB input).
 * ======================================================================== */

#define GUNZIP_STREAM_IN_BUF_SIZE   (256 * 1024)   /* 256KB input buffer */
#define GUNZIP_STREAM_OUT_BUF_SIZE  (4 * 1024 * 1024) /* 4MB output buffer */
#define STREAM_WINDOW_SIZE          (32 * 1024)     /* 32KB sliding window */

/* ========================================================================
 * Incremental CRC32 (no initial/final XOR – caller handles that)
 * ======================================================================== */

static uint32_t crc32_update_inc(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len--)
        crc = crc32_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return crc;
}

/* ========================================================================
 * VFS-backed bitstream reader
 * ======================================================================== */

typedef struct {
    int          fd;
    size_t       read_pos;    /* next file position to read from */
    size_t       data_end;    /* file position marking end of deflate data */
    uint8_t     *buf;         /* input buffer */
    size_t       buf_size;    /* allocated size of input buffer */
    size_t       buf_valid;   /* number of valid bytes in buf */
    size_t       bytepos;     /* current read position within buf */
    unsigned int bitbuf;
    int          bitcnt;
} vfs_bs_t;

static void vfs_bs_init(vfs_bs_t *vbs, int fd, uint8_t *buf, size_t buf_size,
                        size_t data_start, size_t data_end)
{
    vbs->fd        = fd;
    vbs->buf       = buf;
    vbs->buf_size  = buf_size;
    vbs->read_pos  = data_start;
    vbs->data_end  = data_end;
    vbs->buf_valid = 0;
    vbs->bytepos   = 0;
    vbs->bitbuf    = 0;
    vbs->bitcnt    = 0;
}

/* Refill the input buffer from VFS.  Moves remaining data to the start
 * and reads new data after it.  Returns 0 on success, -1 on error. */
static int vfs_bs_refill(vfs_bs_t *vbs)
{
    /* Move unconsumed data to the start of the buffer */
    if (vbs->bytepos > 0) {
        size_t remaining = (vbs->bytepos <= vbs->buf_valid)
                         ? vbs->buf_valid - vbs->bytepos : 0;
        if (remaining > 0)
            memmove(vbs->buf, vbs->buf + vbs->bytepos, remaining);
        vbs->buf_valid = remaining;
        vbs->bytepos   = 0;
    }

    /* Read more data from VFS */
    size_t space    = vbs->buf_size - vbs->buf_valid;
    size_t can_read = vbs->data_end - vbs->read_pos;
    if (can_read == 0 && vbs->buf_valid == 0)
        return -1;
    if (space == 0 || can_read == 0)
        return 0;

    size_t to_read = space < can_read ? space : can_read;
    vfs_seek(vbs->fd, (int64_t)vbs->read_pos, VFS_SEEK_SET);
    int n = vfs_read(vbs->fd, vbs->buf + vbs->buf_valid, to_read);
    if (n <= 0)
        return (vbs->buf_valid > 0) ? 0 : -1;

    vbs->buf_valid += n;
    vbs->read_pos  += n;
    return 0;
}

/* Load more bits into the bitstream register */
static int vfs_bs_load(vfs_bs_t *vbs)
{
    while (vbs->bitcnt <= 24) {
        if (vbs->bytepos >= vbs->buf_valid) {
            if (vfs_bs_refill(vbs) < 0)
                break;
            if (vbs->bytepos >= vbs->buf_valid)
                break;
        }
        vbs->bitbuf |= (unsigned int)vbs->buf[vbs->bytepos] << vbs->bitcnt;
        vbs->bytepos++;
        vbs->bitcnt += 8;
    }
    return (vbs->bitcnt > 0) ? 0 : -1;
}

static int vfs_bs_peek(vfs_bs_t *vbs, int n, unsigned int *val)
{
    if (vbs->bitcnt < n) {
        if (vfs_bs_load(vbs) < 0)
            return -1;
    }
    if (vbs->bitcnt < n)
        return -1;
    *val = vbs->bitbuf & ((1u << n) - 1);
    return 0;
}

static void vfs_bs_drop(vfs_bs_t *vbs, int n)
{
    vbs->bitbuf >>= n;
    vbs->bitcnt -= n;
}

static int vfs_bs_read(vfs_bs_t *vbs, int n, unsigned int *val)
{
    if (vfs_bs_peek(vbs, n, val) < 0)
        return -1;
    vfs_bs_drop(vbs, n);
    return 0;
}

static void vfs_bs_align(vfs_bs_t *vbs)
{
    int discard = vbs->bitcnt & 7;
    if (discard) {
        vbs->bitbuf >>= discard;
        vbs->bitcnt -= discard;
    }
}

/* ========================================================================
 * Streaming output buffer with sliding window
 * ======================================================================== */

typedef struct {
    uint8_t          *data;
    size_t            cap;
    size_t            pos;      /* current write position */
    gunzip_output_cb  cb;
    void             *cb_ctx;
} stream_output_t;

static int stream_out_init(stream_output_t *o, size_t cap,
                           gunzip_output_cb cb, void *ctx)
{
    o->data = (uint8_t *)kmalloc(cap);
    if (!o->data) {
        printf("[gunzip] out of memory allocating %u byte stream output\n",
               (unsigned)cap);
        return GUNZIP_ERR_MEMORY;
    }
    o->cap    = cap;
    o->pos    = 0;
    o->cb     = cb;
    o->cb_ctx = ctx;
    return GUNZIP_OK;
}

static void stream_out_free(stream_output_t *o)
{
    if (o->data) {
        kfree(o->data);
        o->data = NULL;
    }
    o->cap = 0;
    o->pos = 0;
}

/* Flush output, call callback, then keep a sliding window for
 * back-reference support.  If is_final, discard the window. */
static int stream_out_flush(stream_output_t *o, int is_final)
{
    if (o->pos == 0)
        return GUNZIP_OK;

    int rc = o->cb(o->data, o->pos, o->cb_ctx);
    if (rc < 0)
        return rc;

    if (is_final) {
        o->pos = 0;
        return GUNZIP_OK;
    }

    /* Keep sliding window for back-references */
    size_t window = (o->pos > STREAM_WINDOW_SIZE) ? STREAM_WINDOW_SIZE : o->pos;
    if (window > 0 && window < o->pos)
        memmove(o->data, o->data + o->pos - window, window);
    o->pos = window;
    return GUNZIP_OK;
}

/* Ensure the output buffer has room for `additional` bytes.
 * May flush and compact the buffer. */
static int stream_out_ensure(stream_output_t *o, size_t additional)
{
    if (o->pos + additional <= o->cap)
        return GUNZIP_OK;

    int rc = stream_out_flush(o, 0);
    if (rc < 0)
        return rc;

    if (o->pos + additional <= o->cap)
        return GUNZIP_OK;

    return GUNZIP_ERR_OVERFLOW;
}

static int stream_out_write_byte(stream_output_t *o, uint8_t b)
{
    if (o->pos >= o->cap) {
        int rc = stream_out_flush(o, 0);
        if (rc < 0) return rc;
    }
    o->data[o->pos++] = b;
    return GUNZIP_OK;
}

static int stream_out_copy_match(stream_output_t *o, size_t dist, size_t len)
{
    if (dist == 0 || dist > o->pos) {
        printf("[gunzip] stream: invalid back-ref dist=%u pos=%u\n",
               (unsigned)dist, (unsigned)o->pos);
        return GUNZIP_ERR_DATA;
    }

    int rc = stream_out_ensure(o, len);
    if (rc < 0) return rc;

    /* Byte-by-byte copy for overlapping matches */
    size_t src = o->pos - dist;
    for (size_t i = 0; i < len; i++)
        o->data[o->pos++] = o->data[src + i];

    return GUNZIP_OK;
}

/* ========================================================================
 * VFS-streaming inflate helpers
 * ======================================================================== */

/* Huffman decode using VFS bitstream (same algorithm, different types) */
static int huffman_decode_vfs(vfs_bs_t *bs, const huffman_t *h,
                              unsigned int *sym)
{
    int code  = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len <= MAX_BITS; len++) {
        unsigned int bit;
        if (vfs_bs_read(bs, 1, &bit) < 0)
            return GUNZIP_ERR_DATA;
        code = (code << 1) | (int)bit;
        int count = h->counts[len];
        if (code - count < first) {
            *sym = h->symbols[index + (code - first)];
            return GUNZIP_OK;
        }
        index += count;
        first = (first + count) << 1;
    }

    return GUNZIP_ERR_HUFFMAN;
}

/* Inflate a stored (uncompressed) block from VFS bitstream */
static int inflate_stored_block_vfs(vfs_bs_t *vbs, stream_output_t *out)
{
    vfs_bs_align(vbs);
    vbs->bitbuf = 0;
    vbs->bitcnt = 0;

    /* Ensure we have at least 4 bytes for LEN/NLEN */
    if (vbs->bytepos + 4 > vbs->buf_valid) {
        if (vfs_bs_refill(vbs) < 0)
            return GUNZIP_ERR_DATA;
        if (vbs->bytepos + 4 > vbs->buf_valid)
            return GUNZIP_ERR_DATA;
    }

    unsigned int len  = (unsigned int)vbs->buf[vbs->bytepos]
                      | ((unsigned int)vbs->buf[vbs->bytepos + 1] << 8);
    unsigned int nlen = (unsigned int)vbs->buf[vbs->bytepos + 2]
                      | ((unsigned int)vbs->buf[vbs->bytepos + 3] << 8);
    vbs->bytepos += 4;

    if ((len ^ nlen) != 0xffff) {
        printf("[gunzip] stream stored block: len/nlen mismatch\n");
        return GUNZIP_ERR_DATA;
    }

    /* Copy raw data to output in chunks */
    size_t remaining = len;
    while (remaining > 0) {
        if (vbs->bytepos >= vbs->buf_valid) {
            if (vfs_bs_refill(vbs) < 0)
                return GUNZIP_ERR_DATA;
            if (vbs->bytepos >= vbs->buf_valid)
                return GUNZIP_ERR_DATA;
        }
        if (out->pos >= out->cap) {
            int rc = stream_out_flush(out, 0);
            if (rc < 0) return rc;
        }

        size_t in_avail  = vbs->buf_valid - vbs->bytepos;
        size_t out_avail = out->cap - out->pos;
        size_t to_copy   = remaining;
        if (to_copy > in_avail)  to_copy = in_avail;
        if (to_copy > out_avail) to_copy = out_avail;
        if (to_copy == 0) return GUNZIP_ERR_DATA;

        memcpy(out->data + out->pos, vbs->buf + vbs->bytepos, to_copy);
        out->pos      += to_copy;
        vbs->bytepos  += to_copy;
        remaining     -= to_copy;
    }

    return GUNZIP_OK;
}

/* Inflate a fixed Huffman block (type 1) from VFS bitstream */
static int inflate_fixed_block_vfs(vfs_bs_t *vbs, stream_output_t *out)
{
    if (!fixed_tables_built)
        build_fixed_tables();

    for (;;) {
        unsigned int sym;
        int rc = huffman_decode_vfs(vbs, &fixed_lit, &sym);
        if (rc < 0) return rc;

        if (sym < 256) {
            rc = stream_out_write_byte(out, (uint8_t)sym);
            if (rc < 0) return rc;
        } else if (sym == 256) {
            break;
        } else {
            unsigned int len_idx = sym - 257;
            if (len_idx >= 29) return GUNZIP_ERR_DATA;

            size_t length = len_base[len_idx];
            if (len_extra[len_idx] > 0) {
                unsigned int extra;
                if (vfs_bs_read(vbs, len_extra[len_idx], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                length += extra;
            }

            unsigned int dist_sym;
            rc = huffman_decode_vfs(vbs, &fixed_dist, &dist_sym);
            if (rc < 0) return rc;
            if (dist_sym >= 30) return GUNZIP_ERR_DATA;

            size_t distance = dist_base[dist_sym];
            if (dist_extra[dist_sym] > 0) {
                unsigned int extra;
                if (vfs_bs_read(vbs, dist_extra[dist_sym], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                distance += extra;
            }

            rc = stream_out_copy_match(out, distance, length);
            if (rc < 0) return rc;
        }
    }

    return GUNZIP_OK;
}

/* Inflate a dynamic Huffman block (type 2) from VFS bitstream */
static int inflate_dynamic_block_vfs(vfs_bs_t *vbs, stream_output_t *out)
{
    unsigned int hlit, hdist, hclen;
    uint8_t cl_lengths[NUM_CL_CODES];
    uint8_t lit_lengths[NUM_LIT_CODES + NUM_DIST_CODES];
    int i, num_lit, num_dist;

    if (vfs_bs_read(vbs, 5, &hlit)  < 0) return GUNZIP_ERR_DATA;
    if (vfs_bs_read(vbs, 5, &hdist) < 0) return GUNZIP_ERR_DATA;
    if (vfs_bs_read(vbs, 4, &hclen) < 0) return GUNZIP_ERR_DATA;

    hlit  += 257;
    hdist += 1;
    hclen += 4;
    num_lit  = (int)hlit;
    num_dist = (int)hdist;

    if (num_lit > NUM_LIT_CODES || num_dist > NUM_DIST_CODES)
        return GUNZIP_ERR_DATA;

    /* Read code length code lengths */
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (i = 0; i < (int)hclen; i++) {
        unsigned int val;
        if (vfs_bs_read(vbs, 3, &val) < 0)
            return GUNZIP_ERR_DATA;
        cl_lengths[cl_order[i]] = (uint8_t)val;
    }

    huffman_t cl_table;
    int rc = huffman_build(&cl_table, cl_lengths, NUM_CL_CODES);
    if (rc < 0) return rc;

    /* Decode literal/length + distance code lengths */
    int total = num_lit + num_dist;
    memset(lit_lengths, 0, sizeof(lit_lengths));

    i = 0;
    while (i < total) {
        unsigned int sym;
        rc = huffman_decode_vfs(vbs, &cl_table, &sym);
        if (rc < 0) return rc;

        if (sym < 16) {
            lit_lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            unsigned int rep;
            if (vfs_bs_read(vbs, 2, &rep) < 0) return GUNZIP_ERR_DATA;
            rep += 3;
            if (i == 0 || i + (int)rep > total) return GUNZIP_ERR_DATA;
            uint8_t prev = lit_lengths[i - 1];
            while (rep--) lit_lengths[i++] = prev;
        } else if (sym == 17) {
            unsigned int rep;
            if (vfs_bs_read(vbs, 3, &rep) < 0) return GUNZIP_ERR_DATA;
            rep += 3;
            if (i + (int)rep > total) return GUNZIP_ERR_DATA;
            while (rep--) lit_lengths[i++] = 0;
        } else if (sym == 18) {
            unsigned int rep;
            if (vfs_bs_read(vbs, 7, &rep) < 0) return GUNZIP_ERR_DATA;
            rep += 11;
            if (i + (int)rep > total) return GUNZIP_ERR_DATA;
            while (rep--) lit_lengths[i++] = 0;
        } else {
            return GUNZIP_ERR_DATA;
        }
    }

    /* Build Huffman tables */
    huffman_t lit_table;
    rc = huffman_build(&lit_table, lit_lengths, num_lit);
    if (rc < 0) return rc;

    int has_dist = 0;
    for (int j = 0; j < num_dist; j++) {
        if (lit_lengths[num_lit + j] != 0) { has_dist = 1; break; }
    }

    huffman_t dist_table;
    if (has_dist) {
        rc = huffman_build(&dist_table, &lit_lengths[num_lit], num_dist);
        if (rc < 0) return rc;
    } else {
        memset(&dist_table, 0, sizeof(dist_table));
    }

    /* Inflate symbols */
    for (;;) {
        unsigned int sym;
        rc = huffman_decode_vfs(vbs, &lit_table, &sym);
        if (rc < 0) return rc;

        if (sym < 256) {
            rc = stream_out_write_byte(out, (uint8_t)sym);
            if (rc < 0) return rc;
        } else if (sym == 256) {
            break;
        } else {
            unsigned int len_idx = sym - 257;
            if (len_idx >= 29) return GUNZIP_ERR_DATA;

            size_t length = len_base[len_idx];
            if (len_extra[len_idx] > 0) {
                unsigned int extra;
                if (vfs_bs_read(vbs, len_extra[len_idx], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                length += extra;
            }

            unsigned int dist_sym;
            if (!has_dist) return GUNZIP_ERR_DATA;
            rc = huffman_decode_vfs(vbs, &dist_table, &dist_sym);
            if (rc < 0) return rc;
            if (dist_sym >= 30) return GUNZIP_ERR_DATA;

            size_t distance = dist_base[dist_sym];
            if (dist_extra[dist_sym] > 0) {
                unsigned int extra;
                if (vfs_bs_read(vbs, dist_extra[dist_sym], &extra) < 0)
                    return GUNZIP_ERR_DATA;
                distance += extra;
            }

            rc = stream_out_copy_match(out, distance, length);
            if (rc < 0) return rc;
        }
    }

    return GUNZIP_OK;
}

/* Main inflate loop using VFS bitstream and streaming output */
static int inflate_stream_vfs(vfs_bs_t *vbs, stream_output_t *out)
{
    int bfinal;

    do {
        unsigned int final_bit, type_bits;

        if (vfs_bs_read(vbs, 1, &final_bit) < 0) {
            printf("[gunzip] stream: failed to read BFINAL\n");
            return GUNZIP_ERR_DATA;
        }
        if (vfs_bs_read(vbs, 2, &type_bits) < 0) {
            printf("[gunzip] stream: failed to read BTYPE\n");
            return GUNZIP_ERR_DATA;
        }

        bfinal = (int)final_bit;
        int rc;

        switch (type_bits) {
        case 0: rc = inflate_stored_block_vfs(vbs, out);  break;
        case 1: rc = inflate_fixed_block_vfs(vbs, out);   break;
        case 2: rc = inflate_dynamic_block_vfs(vbs, out); break;
        default:
            printf("[gunzip] stream: reserved block type 3\n");
            return GUNZIP_ERR_BLOCK;
        }

        if (rc < 0) return rc;

    } while (!bfinal);

    return GUNZIP_OK;
}

/* ========================================================================
 * Internal context for CRC32 verification wrapping the user callback
 * ======================================================================== */

typedef struct {
    gunzip_output_cb user_cb;
    void            *user_ctx;
    uint32_t         crc;        /* running CRC (init 0xFFFFFFFF, finalize with XOR) */
    size_t           total_out;  /* total decompressed bytes */
} gz_stream_internal_t;

static int gz_stream_cb_wrapper(const void *data, size_t len, void *ctx)
{
    gz_stream_internal_t *ictx = (gz_stream_internal_t *)ctx;
    ictx->crc = crc32_update_inc(ictx->crc, data, len);
    ictx->total_out += len;
    return ictx->user_cb(data, len, ictx->user_ctx);
}

/* ========================================================================
 * gunzip_stream_vfs - Public streaming API
 * ======================================================================== */

int gunzip_stream_vfs(int fd, size_t gz_offset, size_t gz_size,
                      gunzip_output_cb output_cb, void *ctx)
{
    uint8_t tmp[264];  /* enough for gzip header + FEXTRA/FNAME/FCOMMENT/FHCRC */
    size_t pos;
    size_t deflate_start, deflate_end;

    if (!output_cb || gz_size < 18)  /* 10 header + 8 trailer minimum */
        return GUNZIP_ERR_DATA;

    /* ---- 1. Read gzip trailer (last 8 bytes) ---- */
    uint8_t trailer[8];
    vfs_seek(fd, (int64_t)(gz_offset + gz_size - 8), VFS_SEEK_SET);
    if (vfs_read(fd, trailer, 8) != 8)
        return GUNZIP_ERR_DATA;

    uint32_t expected_crc = (uint32_t)trailer[0]
                          | ((uint32_t)trailer[1] << 8)
                          | ((uint32_t)trailer[2] << 16)
                          | ((uint32_t)trailer[3] << 24);
    uint32_t expected_size = (uint32_t)trailer[4]
                           | ((uint32_t)trailer[5] << 8)
                           | ((uint32_t)trailer[6] << 16)
                           | ((uint32_t)trailer[7] << 24);

    /* ---- 2. Read and parse gzip header ---- */
    vfs_seek(fd, (int64_t)gz_offset, VFS_SEEK_SET);
    if (vfs_read(fd, tmp, 10) != 10)
        return GUNZIP_ERR_DATA;

    if (tmp[0] != GZIP_MAGIC_0 || tmp[1] != GZIP_MAGIC_1) {
        printf("[gunzip] stream: bad gzip magic\n");
        return GUNZIP_ERR_MAGIC;
    }
    if (tmp[2] != GZIP_METHOD_DEFLATE) {
        printf("[gunzip] stream: unsupported method %u\n", tmp[2]);
        return GUNZIP_ERR_METHOD;
    }

    uint8_t flags = tmp[3];
    if (flags & 0xe0) {
        printf("[gunzip] stream: reserved flag bits\n");
        return GUNZIP_ERR_FLAGS;
    }

    pos = 10;

    /* FEXTRA */
    if (flags & GZIP_FLAG_FEXTRA) {
        uint8_t xb[2];
        vfs_seek(fd, (int64_t)(gz_offset + pos), VFS_SEEK_SET);
        if (vfs_read(fd, xb, 2) != 2) return GUNZIP_ERR_DATA;
        uint16_t xlen = (uint16_t)xb[0] | ((uint16_t)xb[1] << 8);
        pos += 2 + xlen;
    }

    /* FNAME */
    if (flags & GZIP_FLAG_FNAME) {
        vfs_seek(fd, (int64_t)(gz_offset + pos), VFS_SEEK_SET);
        uint8_t c;
        do {
            if (vfs_read(fd, &c, 1) != 1) return GUNZIP_ERR_DATA;
            pos++;
        } while (c != '\0');
    }

    /* FCOMMENT */
    if (flags & GZIP_FLAG_FCOMMENT) {
        vfs_seek(fd, (int64_t)(gz_offset + pos), VFS_SEEK_SET);
        uint8_t c;
        do {
            if (vfs_read(fd, &c, 1) != 1) return GUNZIP_ERR_DATA;
            pos++;
        } while (c != '\0');
    }

    /* FHCRC */
    if (flags & GZIP_FLAG_FHCRC) {
        pos += 2;
    }

    deflate_start = gz_offset + pos;
    deflate_end   = gz_offset + gz_size - 8;

    if (deflate_start >= deflate_end) {
        printf("[gunzip] stream: no compressed data after header\n");
        return GUNZIP_ERR_DATA;
    }

    /* ---- 3. Allocate buffers ---- */
    uint8_t *in_buf = (uint8_t *)kmalloc(GUNZIP_STREAM_IN_BUF_SIZE);
    if (!in_buf) return GUNZIP_ERR_MEMORY;

    gz_stream_internal_t ictx;
    ictx.user_cb   = output_cb;
    ictx.user_ctx  = ctx;
    ictx.crc       = 0xFFFFFFFF;
    ictx.total_out = 0;

    stream_output_t out;
    int rc = stream_out_init(&out, GUNZIP_STREAM_OUT_BUF_SIZE,
                             gz_stream_cb_wrapper, &ictx);
    if (rc < 0) {
        kfree(in_buf);
        return rc;
    }

    /* ---- 4. Initialize VFS bitstream and inflate ---- */
    vfs_bs_t vbs;
    vfs_bs_init(&vbs, fd, in_buf, GUNZIP_STREAM_IN_BUF_SIZE,
                deflate_start, deflate_end);

    rc = inflate_stream_vfs(&vbs, &out);
    if (rc < 0) {
        stream_out_free(&out);
        kfree(in_buf);
        return rc;
    }

    /* ---- 5. Flush remaining output ---- */
    rc = stream_out_flush(&out, 1);
    stream_out_free(&out);
    kfree(in_buf);
    if (rc < 0) return rc;

    /* ---- 6. Verify CRC32 and ISIZE ---- */
    ictx.crc ^= 0xFFFFFFFF;

    if (ictx.crc != expected_crc) {
        printf("[gunzip] stream: CRC32 mismatch (expected 0x%08x got 0x%08x)\n",
               expected_crc, ictx.crc);
        return GUNZIP_ERR_CRC;
    }

    if ((uint32_t)(ictx.total_out & 0xFFFFFFFF) != expected_size) {
        printf("[gunzip] stream: ISIZE mismatch (expected %u got %u)\n",
               expected_size, (unsigned)(ictx.total_out & 0xFFFFFFFF));
        return GUNZIP_ERR_SIZE;
    }

    return GUNZIP_OK;
}
