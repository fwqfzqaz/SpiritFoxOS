/* SpiritFoxOS libc - Character classification (musl-style table lookup) */

#include <ctype.h>

/* Bit flags for the classification table */
#define _U  0x01  /* upper */
#define _L  0x02  /* lower */
#define _D  0x04  /* digit */
#define _S  0x08  /* space */
#define _P  0x10  /* punct */
#define _C  0x20  /* cntrl */
#define _X  0x40  /* hex digit */
#define _G  0x80  /* graph (printable, non-space) */

static const unsigned char _ctype_table[128] = {
    /* 0-7 */   _C, _C, _C, _C, _C, _C, _C, _C,
    /* 8-15 */  _C, _C|_S, _C|_S, _C|_S, _C|_S, _C|_S, _C, _C,
    /* 16-23 */ _C, _C, _C, _C, _C, _C, _C, _C,
    /* 24-31 */ _C, _C, _C, _C, _C, _C, _C, _C,
    /* 32 */    _S,
    /* 33-47 */ _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G,
                _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G,
    /* 48-57 */ _D|_X|_G, _D|_X|_G, _D|_X|_G, _D|_X|_G, _D|_X|_G,
                _D|_X|_G, _D|_X|_G, _D|_X|_G, _D|_X|_G, _D|_X|_G,
    /* 58-64 */ _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G,
    /* 65-70 */ _U|_X|_G, _U|_X|_G, _U|_X|_G, _U|_X|_G, _U|_X|_G, _U|_X|_G,
    /* 71-90 */ _U|_G, _U|_G, _U|_G, _U|_G, _U|_G, _U|_G, _U|_G, _U|_G,
                _U|_G, _U|_G, _U|_G, _U|_G, _U|_G, _U|_G, _U|_G, _U|_G,
                _U|_G, _U|_G, _U|_G, _U|_G,
    /* 91-96 */ _P|_G, _P|_G, _P|_G, _P|_G, _P|_G, _P|_G,
    /* 97-102 */ _L|_X|_G, _L|_X|_G, _L|_X|_G, _L|_X|_G, _L|_X|_G, _L|_X|_G,
    /* 103-122 */ _L|_G, _L|_G, _L|_G, _L|_G, _L|_G, _L|_G, _L|_G, _L|_G,
                 _L|_G, _L|_G, _L|_G, _L|_G, _L|_G, _L|_G, _L|_G, _L|_G,
                 _L|_G, _L|_G, _L|_G, _L|_G,
    /* 123-127 */ _P|_G, _P|_G, _P|_G, _P|_G, _C
};

#define _ctype(c) ((unsigned)(c) < 128 ? _ctype_table[(unsigned char)(c)] : 0)

int isalpha(int c) { return _ctype(c) & (_U | _L); }
int isdigit(int c) { return _ctype(c) & _D; }
int isalnum(int c) { return _ctype(c) & (_U | _L | _D); }
int isspace(int c) { return _ctype(c) & _S; }
int isupper(int c) { return _ctype(c) & _U; }
int islower(int c) { return _ctype(c) & _L; }
int isprint(int c) { return _ctype(c) & (_G | _S); }
int ispunct(int c) { return _ctype(c) & _P; }
int isxdigit(int c) { return _ctype(c) & _X; }
int isgraph(int c) { return _ctype(c) & _G; }
int iscntrl(int c) { return _ctype(c) & _C; }

int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
