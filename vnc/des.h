/*---------------------------------------------------------------------------*/
/* Copyright (c) 2026 B. C. Services                                         */
/* Licensed under GPL-3.0.                                                    */
/*---------------------------------------------------------------------------*/
/* des.h:                                                                    */
/*                                                                           */
/* A self-contained single-block DES, present for one purpose only: the RFB  */
/* "VNC Authentication" security type (RFC 6143 section 7.2.2), which        */
/* challenge-responds with DES.  Rolling our own (one file, no libcrypto /   */
/* no OpenSSL) keeps the server small and self-contained.                    */
/*                                                                           */
/* This is NOT a general crypto library and must not be used as one.  DES is */
/* obsolete and the VNC scheme around it is weak; it is used here solely     */
/* because it is the credential exchange the VNC servers (x11vnc / wayvnc /  */
/* the like) already speak, on an explicitly LAN-trusted, unencrypted link.  */
/*                                                                           */
/* The VNC quirk this encodes: the DES key is the first 8 password bytes,    */
/* but each byte's bit order is REVERSED first (a historical artefact of the */
/* original AT&T VNC DES).  VDES_VncEncryptChallenge does that reversal.      */
/*---------------------------------------------------------------------------*/

#ifndef GRUMPY_VNC_DES_H
#define GRUMPY_VNC_DES_H

#include <stddef.h>

/*--------------------------------------------------------------------------*/
/* VDES_Context:                                                             */
/*                                                                          */
/* Holds the 16 expanded 48-bit round subkeys derived from one 8-byte key.  */
/* Kept as a caller-owned value struct rather than file-global state so the */
/* module has no hidden state and is trivially re-entrant.  Each subkey is  */
/* stored one bit per int (0 or 1) for clarity over compactness - DES runs  */
/* twice per authentication, so speed is irrelevant and correctness is all. */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int des_arrSubkeys[16][48];             // 16 rounds x 48 key bits, MSB-first
} VDES_Context;

/*--------------------------------------------------------------------------*/
/* VDES_SetKey:                                                              */
/*                                                                          */
/* Runs the DES key schedule (PC-1, the per-round left rotations, PC-2) to  */
/* fill the 16 round subkeys in the context from an 8-byte key.  The key's  */
/* parity bits are ignored by the schedule, as in standard DES.             */
/*                                                                          */
/* Arguments:                                                               */
/*     pCtx  : context to populate; overwritten in full.                    */
/*     pKey8 : exactly 8 key bytes (already bit-reversed for VNC, if that   */
/*             is the caller's scheme - this function does no reversal).    */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes the subkeys into *pCtx.                                */
/*--------------------------------------------------------------------------*/

void VDES_SetKey(VDES_Context *pCtx, const unsigned char *pKey8);

/*---------------------------------------------------------------------------*/
/* VDES_EncryptBlock:                                                         */
/*                                                                           */
/* Encrypts one 8-byte block (ECB, no padding, no chaining) with the subkeys */
/* already loaded in the context: initial permutation, 16 Feistel rounds,    */
/* the L/R swap, final permutation.                                          */
/*                                                                           */
/* Arguments:                                                                */
/*     pCtx  : context whose subkeys were set by VDES_SetKey.                 */
/*     pIn8  : 8 plaintext bytes.                                            */
/*     pOut8 : 8-byte buffer to receive the ciphertext (may not alias pIn8). */
/*                                                                           */
/* Returns:                                                                  */
/*     void : writes 8 bytes to pOut8.                                       */
/*---------------------------------------------------------------------------*/

void VDES_EncryptBlock(const VDES_Context *pCtx, const unsigned char *pIn8,
                      unsigned char *pOut8);

/*---------------------------------------------------------------------------*/
/* VDES_VncEncryptChallenge:                                                  */
/*                                                                           */
/* Performs the full VNC-Authentication response: take the password (first 8 */
/* bytes, null-padded if shorter), REVERSE the bits of each of those 8 bytes */
/* to form the DES key, then DES-ECB-encrypt the server's 16-byte challenge  */
/* as two independent 8-byte blocks, writing the 16-byte response.           */
/*                                                                           */
/* Arguments:                                                                */
/*     pPassword : password bytes (not necessarily NUL-terminated use).      */
/*     iPassLen  : number of password bytes to consider (only first 8 used). */
/*     pIn16     : the 16-byte challenge from the server.                    */
/*     pOut16    : 16-byte buffer for the response (may not alias pIn16).    */
/*                                                                           */
/* Returns:                                                                  */
/*     void : writes 16 bytes to pOut16.                                     */
/*---------------------------------------------------------------------------*/

void VDES_VncEncryptChallenge(const unsigned char *pPassword, size_t iPassLen,
                             const unsigned char *pIn16, unsigned char *pOut16);

#endif // GRUMPY_VNC_DES_H
