/*
 * Fallback for builds without a compatible Fortran compiler (e.g. MSVC on
 * Windows). The native JT65/WSPR codecs are WSJT-X Fortran; without them
 * every entry point reports failure (rc = -1) and the C++ callers treat the
 * mode as unavailable instead of failing to link.
 */
#include <string.h>

void aethersdr_jt65_encode(const char message[23], int tones[126], char normalized[23],
                           int* itype, int* rc)
{
    (void)message;
    memset(tones, 0, sizeof(int) * 126);
    memset(normalized, 0, 23);
    *itype = 0;
    *rc = -1;
}

void aethersdr_jt65_decode(const int symbols[63], const int erased[63], char message[23],
                           int* nerr, int* rc)
{
    (void)symbols;
    (void)erased;
    memset(message, 0, 23);
    *nerr = -1;
    *rc = -1;
}

void aethersdr_wspr_encode(const char message[23], int tones[162], char normalized[23], int* rc)
{
    (void)message;
    memset(tones, 0, sizeof(int) * 162);
    memset(normalized, 0, 23);
    *rc = -1;
}

void aethersdr_wspr_decode(const int softSymbols[162], char message[23], int* metric,
                           int* ncycles, int* rc)
{
    (void)softSymbols;
    memset(message, 0, 23);
    *metric = 0;
    *ncycles = 0;
    *rc = -1;
}
