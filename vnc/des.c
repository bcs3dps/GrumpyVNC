/*--------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                        */
/* Licensed under GPL-3.0.                                                   */
/*--------------------------------------------------------------------------*/
/* des.c:                                                                   */
/*                                                                          */
/* Textbook single-block DES (FIPS 46-3) plus the VNC key bit-reversal      */
/* wrapper, implementing the RFB VNC-Authentication response declared in    */
/* des.h.  Bits are carried one-per-int, MSB first, throughout: it is the   */
/* least error-prone representation and DES runs only twice per login, so   */
/* the inefficiency costs nothing that matters.                             */
/*                                                                          */
/* The permutation, expansion, S-box, PC-1, PC-2 and rotation tables below  */
/* are the standard DES constants (FIPS 46-3); they are the external        */
/* specification this file implements and are cited as such.                */
/*--------------------------------------------------------------------------*/

#include "des.h"

#include <string.h>

/*----------------------------------------------------------------------------*/
/* IP - Initial Permutation (FIPS 46-3): 64 -> 64.  Each entry is the 1-based */
/* source bit position for that output position.                              */
/*----------------------------------------------------------------------------*/

static const int VDES_arrIP[64] =
{
    58, 50, 42, 34, 26, 18, 10,  2,
    60, 52, 44, 36, 28, 20, 12,  4,
    62, 54, 46, 38, 30, 22, 14,  6,
    64, 56, 48, 40, 32, 24, 16,  8,
    57, 49, 41, 33, 25, 17,  9,  1,
    59, 51, 43, 35, 27, 19, 11,  3,
    61, 53, 45, 37, 29, 21, 13,  5,
    63, 55, 47, 39, 31, 23, 15,  7
};

/*--------------------------------------------------------------------------*/
/* FP - Final Permutation, the inverse of IP (FIPS 46-3): 64 -> 64.         */
/*--------------------------------------------------------------------------*/

static const int VDES_arrFP[64] =
{
    40,  8, 48, 16, 56, 24, 64, 32,
    39,  7, 47, 15, 55, 23, 63, 31,
    38,  6, 46, 14, 54, 22, 62, 30,
    37,  5, 45, 13, 53, 21, 61, 29,
    36,  4, 44, 12, 52, 20, 60, 28,
    35,  3, 43, 11, 51, 19, 59, 27,
    34,  2, 42, 10, 50, 18, 58, 26,
    33,  1, 41,  9, 49, 17, 57, 25
};

/*--------------------------------------------------------------------------*/
/* E - Expansion (FIPS 46-3): 32 -> 48, expanding the right half before the */
/* subkey XOR.                                                              */
/*--------------------------------------------------------------------------*/

static const int VDES_arrE[48] =
{
    32,  1,  2,  3,  4,  5,
     4,  5,  6,  7,  8,  9,
     8,  9, 10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32,  1
};

/*---------------------------------------------------------------------------*/
/* P - Permutation (FIPS 46-3): 32 -> 32, applied to the S-box output inside */
/* the Feistel function.                                                     */
/*---------------------------------------------------------------------------*/

static const int VDES_arrP[32] =
{
    16,  7, 20, 21, 29, 12, 28, 17,
     1, 15, 23, 26,  5, 18, 31, 10,
     2,  8, 24, 14, 32, 27,  3,  9,
    19, 13, 30,  6, 22, 11,  4, 25
};

/*--------------------------------------------------------------------------*/
/* PC-1 - Permuted Choice 1 (FIPS 46-3): 64 -> 56, dropping the key parity  */
/* bits and reordering into the two 28-bit halves C and D.                  */
/*--------------------------------------------------------------------------*/

static const int VDES_arrPC1[56] =
{
    57, 49, 41, 33, 25, 17,  9,
     1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27,
    19, 11,  3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
     7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29,
    21, 13,  5, 28, 20, 12,  4
};

/*--------------------------------------------------------------------------*/
/* PC-2 - Permuted Choice 2 (FIPS 46-3): 56 -> 48, selecting each round's   */
/* subkey from the rotated C and D halves.                                  */
/*--------------------------------------------------------------------------*/

static const int VDES_arrPC2[48] =
{
    14, 17, 11, 24,  1,  5,
     3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8,
    16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32
};

/*--------------------------------------------------------------------------*/
/* Per-round left-rotation counts for the C and D key halves (FIPS 46-3).   */
/*--------------------------------------------------------------------------*/

static const int VDES_arrShifts[16] =
{
    1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1
};

/*----------------------------------------------------------------------------*/
/* The eight S-boxes (FIPS 46-3), each flattened to 64 entries in row order.  */
/* For a 6-bit group b1..b6: row = (b1<<1)|b6, column = b2..b5, and the value */
/* is VDES_arrS[box][row*16 + column], a 4-bit substitution.                   */
/*----------------------------------------------------------------------------*/

static const int VDES_arrS[8][64] =
{
    /* S1 */
    {
        14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,
         0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,
         4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0,
        15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13
    },
    /* S2 */
    {
        15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,
         3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,
         0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15,
        13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9
    },
    /* S3 */
    {
        10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,
        13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,
        13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
         1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12
    },
    /* S4 */
    {
         7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,
        13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,
        10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
         3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14
    },
    /* S5 */
    {
         2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9,
        14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6,
         4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14,
        11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3
    },
    /* S6 */
    {
        12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
        10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
         9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
         4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13
    },
    /* S7 */
    {
         4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
        13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
         1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
         6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12
    },
    /* S8 */
    {
        13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
         1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
         7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
         2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11
    }
};

/*--------------------------------------------------------------------------*/
/* VDES_Permute:                                                             */
/*                                                                          */
/* Generic bit permutation: out[i] = in[table[i] - 1] for i in 0..iOutLen.  */
/* Used for IP, FP, E, P, PC-1 and PC-2 alike.  Local to this file.         */
/*                                                                          */
/* Arguments:                                                               */
/*     pIn     : source bit array (0/1 per element).                        */
/*     pOut    : destination bit array of iOutLen elements.                 */
/*     pTable  : 1-based source positions, one per output bit.              */
/*     iOutLen : number of output bits (length of pTable and pOut).         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : fills pOut.                                                   */
/*--------------------------------------------------------------------------*/

static void VDES_Permute(const int *pIn, int *pOut, const int *pTable, int iOutLen)
{
    int i;

    for (i = 0; i < iOutLen; i++)
    {   /* One output bit per iteration; table entries are 1-based. */
        pOut[i] = pIn[pTable[i] - 1];
    }
}

/*---------------------------------------------------------------------------*/
/* VDES_BytesToBits:                                                          */
/*                                                                           */
/* Expands iNumBytes bytes into iNumBytes*8 bits, most-significant bit first */
/* within each byte.  Local to this file.                                    */
/*                                                                           */
/* Arguments:                                                                */
/*     pBytes    : source bytes.                                             */
/*     iNumBytes : how many bytes to expand.                                 */
/*     pBits     : destination array of iNumBytes*8 ints.                    */
/*                                                                           */
/* Returns:                                                                  */
/*     void : fills pBits.                                                   */
/*---------------------------------------------------------------------------*/

static void VDES_BytesToBits(const unsigned char *pBytes, int iNumBytes, int *pBits)
{
    int i;
    int j;

    for (i = 0; i < iNumBytes; i++)
    {   /* One source byte per iteration. */
        for (j = 0; j < 8; j++)
        {   /* One bit per iteration, MSB first: index 0 is the high bit. */
            pBits[(i * 8) + j] = (pBytes[i] >> (7 - j)) & 0x01;
        }
    }
}

/*---------------------------------------------------------------------------*/
/* VDES_BitsToBytes:                                                          */
/*                                                                           */
/* Packs iNumBytes*8 bits back into iNumBytes bytes, MSB first - the inverse */
/* of VDES_BytesToBits.  Local to this file.                                  */
/*                                                                           */
/* Arguments:                                                                */
/*     pBits     : source bit array of iNumBytes*8 ints (each 0 or 1).       */
/*     iNumBytes : how many bytes to produce.                                */
/*     pBytes    : destination byte buffer.                                  */
/*                                                                           */
/* Returns:                                                                  */
/*     void : fills pBytes.                                                  */
/*---------------------------------------------------------------------------*/

static void VDES_BitsToBytes(const int *pBits, int iNumBytes, unsigned char *pBytes)
{
    int i;
    int j;
    int iByte;

    for (i = 0; i < iNumBytes; i++)
    {   /* One output byte per iteration. */
        iByte = 0;

        for (j = 0; j < 8; j++)
        {   /* One bit per iteration, MSB first: index 0 is the high bit. */
            iByte = (iByte << 1) | (pBits[(i * 8) + j] & 0x01);
        }

        pBytes[i] = (unsigned char)iByte;
    }
}

/*--------------------------------------------------------------------------*/
/* VDES_RotateLeft28:                                                        */
/*                                                                          */
/* Rotates a 28-bit half (as a 28-int array) left by iCount positions, in   */
/* place, for the key schedule.  Local to this file.                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pHalf  : 28-element bit array, modified in place.                    */
/*     iCount : number of left-rotate positions (1 or 2 in DES).            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : rotates pHalf in place.                                       */
/*--------------------------------------------------------------------------*/

static void VDES_RotateLeft28(int *pHalf, int iCount)
{
    int arrTmp[28];                            // scratch for the rotated copy
    int i;

    for (i = 0; i < 28; i++)
    {   /* One rotated element per iteration: i takes (i + iCount) mod 28. */
        arrTmp[i] = pHalf[(i + iCount) % 28];
    }

    /* Copy the rotated result back over the source half. */
    memcpy(pHalf, arrTmp, sizeof(arrTmp));
}

/*--------------------------------------------------------------------------*/
/* VDES_SetKey:                                                              */
/*                                                                          */
/* Standard DES key schedule.  See des.h for the contract.                  */
/*                                                                          */
/* Arguments:                                                               */
/*     pCtx  : context to fill.                                             */
/*     pKey8 : 8 key bytes.                                                 */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes 16 subkeys into *pCtx.                                 */
/*--------------------------------------------------------------------------*/

void VDES_SetKey(VDES_Context *pCtx, const unsigned char *pKey8)
{
    int arrKeyBits[64];                        // the 64 raw key bits
    int arrPc1[56];                            // key after PC-1 (parity dropped)
    int arrC[28];                              // left half of the schedule
    int arrD[28];                              // right half of the schedule
    int arrCD[56];                             // recombined halves before PC-2
    int iRound;
    int i;

    /* Expand the key bytes to bits, then apply PC-1 to get the 56-bit key. */
    VDES_BytesToBits(pKey8, 8, arrKeyBits);
    VDES_Permute(arrKeyBits, arrPc1, VDES_arrPC1, 56);

    /* Split the 56 bits into the two 28-bit halves C0 and D0. */
    for (i = 0; i < 28; i++)
    {   /* One bit per iteration: the first 28 form C0, the next 28 D0. */
        arrC[i] = arrPc1[i];
        arrD[i] = arrPc1[i + 28];
    }

    for (iRound = 0; iRound < 16; iRound++)
    {   /* Each round rotates both halves, then selects the subkey via PC-2. */

        /* Rotate C and D left by this round's schedule amount. */
        VDES_RotateLeft28(arrC, VDES_arrShifts[iRound]);
        VDES_RotateLeft28(arrD, VDES_arrShifts[iRound]);

        /* Recombine into a 56-bit value for PC-2. */
        for (i = 0; i < 28; i++)
        {   /* One bit per iteration: C fills the first 28, D the next 28. */
            arrCD[i]      = arrC[i];
            arrCD[i + 28] = arrD[i];
        }

        /* PC-2 selects the 48-bit round subkey. */
        VDES_Permute(arrCD, pCtx->des_arrSubkeys[iRound], VDES_arrPC2, 48);
    }
}

/*----------------------------------------------------------------------------*/
/* VDES_Feistel:                                                               */
/*                                                                            */
/* The DES round function f(R, K): expand R to 48 bits, XOR the round subkey, */
/* substitute through the eight S-boxes to 32 bits, and permute with P.       */
/* Local to this file.                                                        */
/*                                                                            */
/* Arguments:                                                                 */
/*     pRight   : the 32-bit right half (bit array).                          */
/*     pSubkey  : the 48-bit round subkey (bit array).                        */
/*     pOut32   : destination 32-bit result (bit array).                      */
/*                                                                            */
/* Returns:                                                                   */
/*     void : fills pOut32.                                                   */
/*----------------------------------------------------------------------------*/

static void VDES_Feistel(const int *pRight, const int *pSubkey, int *pOut32)
{
    int arrExpanded[48];                       // R after the E expansion
    int arrSboxIn[48];                         // expanded R XOR subkey
    int arrSboxOut[32];                        // S-box substitution output
    int iGroup;
    int iRow;
    int iCol;
    int iVal;
    int j;

    /* Expand the 32-bit right half to 48 bits. */
    VDES_Permute(pRight, arrExpanded, VDES_arrE, 48);

    /* XOR with the round subkey. */
    for (j = 0; j < 48; j++)
    {   /* One bit per iteration: expanded R XOR subkey. */
        arrSboxIn[j] = arrExpanded[j] ^ pSubkey[j];
    }

    for (iGroup = 0; iGroup < 8; iGroup++)
    {   /* Each 6-bit group indexes one S-box and yields 4 output bits. */

        /* Row is the outer two bits (b1,b6); column is the inner four. */
        iRow = (arrSboxIn[iGroup * 6 + 0] << 1) | arrSboxIn[iGroup * 6 + 5];
        iCol = (arrSboxIn[iGroup * 6 + 1] << 3) |
               (arrSboxIn[iGroup * 6 + 2] << 2) |
               (arrSboxIn[iGroup * 6 + 3] << 1) |
               (arrSboxIn[iGroup * 6 + 4]);

        /* Look up the 4-bit substitution value for this box. */
        iVal = VDES_arrS[iGroup][(iRow * 16) + iCol];

        /* Emit the 4 value bits, MSB first, into the S-box output array. */
        for (j = 0; j < 4; j++)
        {   /* One value bit per iteration, MSB first. */
            arrSboxOut[(iGroup * 4) + j] = (iVal >> (3 - j)) & 0x01;
        }
    }

    /* Permute the 32-bit S-box output with P to finish the round function. */
    VDES_Permute(arrSboxOut, pOut32, VDES_arrP, 32);
}

/*--------------------------------------------------------------------------*/
/* VDES_EncryptBlock:                                                        */
/*                                                                          */
/* Standard 16-round DES block encryption.  See des.h for the contract.     */
/*                                                                          */
/* Arguments:                                                               */
/*     pCtx  : context with subkeys set.                                    */
/*     pIn8  : 8 plaintext bytes.                                           */
/*     pOut8 : 8-byte ciphertext buffer.                                    */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes 8 bytes to pOut8.                                      */
/*--------------------------------------------------------------------------*/

void VDES_EncryptBlock(const VDES_Context *pCtx, const unsigned char *pIn8,
                      unsigned char *pOut8)
{
    int arrInBits[64];                         // plaintext bits
    int arrPermuted[64];                       // after the initial permutation
    int arrLeft[32];                           // current left half
    int arrRight[32];                          // current right half
    int arrF[32];                              // round-function output
    int arrTmp[32];                            // holds the old right half
    int arrPreOut[64];                         // R16 || L16 before FP
    int iRound;
    int j;

    /* Load the block and apply the initial permutation. */
    VDES_BytesToBits(pIn8, 8, arrInBits);
    VDES_Permute(arrInBits, arrPermuted, VDES_arrIP, 64);

    /* Split into L0 (high 32) and R0 (low 32). */
    for (j = 0; j < 32; j++)
    {   /* One bit per iteration: the first 32 form L0, the next 32 R0. */
        arrLeft[j]  = arrPermuted[j];
        arrRight[j] = arrPermuted[j + 32];
    }

    for (iRound = 0; iRound < 16; iRound++)
    {   /* Feistel: L(i) = R(i-1); R(i) = L(i-1) XOR f(R(i-1), K(i)). */

        /* Keep the old right half; it becomes the new left half. */
        memcpy(arrTmp, arrRight, sizeof(arrTmp));

        /* Compute the round function on the old right half. */
        VDES_Feistel(arrRight, pCtx->des_arrSubkeys[iRound], arrF);

        /* New right = old left XOR f(). */
        for (j = 0; j < 32; j++)
        {   /* One bit per iteration: new right = old left XOR f(). */
            arrRight[j] = arrLeft[j] ^ arrF[j];
        }

        /* New left = old right. */
        memcpy(arrLeft, arrTmp, sizeof(arrLeft));
    }

    /* Pre-output is R16 || L16 (the halves are swapped before FP). */
    for (j = 0; j < 32; j++)
    {   /* One bit per iteration: right half first, then left half. */
        arrPreOut[j]      = arrRight[j];
        arrPreOut[j + 32] = arrLeft[j];
    }

    /* Final permutation, then pack back to bytes. */
    VDES_Permute(arrPreOut, arrInBits, VDES_arrFP, 64);
    VDES_BitsToBytes(arrInBits, 8, pOut8);
}

/*--------------------------------------------------------------------------*/
/* VDES_ReverseByteBits:                                                     */
/*                                                                          */
/* Reverses the bit order of one byte (bit 0 <-> bit 7, etc).  This is the  */
/* VNC-specific transform applied to each password byte before it becomes a */
/* DES key byte.  Local to this file.                                       */
/*                                                                          */
/* Arguments:                                                               */
/*     ucByte : the byte to reverse.                                        */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned char : the bit-reversed byte.                               */
/*--------------------------------------------------------------------------*/

static unsigned char VDES_ReverseByteBits(unsigned char ucByte)
{
    unsigned char ucOut;                       // accumulates the reversed bits
    int           j;

    ucOut = 0;

    for (j = 0; j < 8; j++)
    {   /* One input bit per iteration: bit j moves to bit (7-j). */
        if ((ucByte & (1 << j)) != 0)
        {   /* Source bit set: set the mirrored destination bit. */
            ucOut |= (unsigned char)(1 << (7 - j));
        }
    }

    /* Return the mirrored byte for use as a DES key byte. */
    return(ucOut);
}

/*---------------------------------------------------------------------------*/
/* VDES_VncEncryptChallenge:                                                  */
/*                                                                           */
/* Full VNC-Authentication response.  See des.h for the contract.            */
/*                                                                           */
/* Arguments:                                                                */
/*     pPassword : password bytes.                                           */
/*     iPassLen  : length of the password (only the first 8 bytes are used). */
/*     pIn16     : 16-byte server challenge.                                 */
/*     pOut16    : 16-byte response buffer.                                  */
/*                                                                           */
/* Returns:                                                                  */
/*     void : writes 16 bytes to pOut16.                                     */
/*---------------------------------------------------------------------------*/

void VDES_VncEncryptChallenge(const unsigned char *pPassword, size_t iPassLen,
                             const unsigned char *pIn16, unsigned char *pOut16)
{
    unsigned char arrKey[8];                   // the bit-reversed DES key
    VDES_Context   ctx;                         // subkeys for that key
    int           i;

    /* Build the 8-byte key: first 8 password bytes (NUL-padded), each byte   */
    /* bit-reversed per the VNC scheme.                                       */
    for (i = 0; i < 8; i++)
    {   /* One key byte per iteration: password byte or NUL, bit-reversed. */
        unsigned char ucSrc;

        if ((size_t)i < iPassLen)
        {   /* Real password byte at this position. */
            ucSrc = pPassword[i];
        }
        else
        {   /* Past the end of the password: pad with NUL. */
            ucSrc = 0;
        }

        arrKey[i] = VDES_ReverseByteBits(ucSrc);
    }

    /* Schedule the subkeys once, then encrypt both challenge halves ECB. */
    VDES_SetKey(&ctx, arrKey);
    VDES_EncryptBlock(&ctx, &pIn16[0], &pOut16[0]);
    VDES_EncryptBlock(&ctx, &pIn16[8], &pOut16[8]);

    /* Scrub the derived key material from the stack before returning. */
    memset(arrKey, 0, sizeof(arrKey));
    memset(&ctx, 0, sizeof(ctx));
}
