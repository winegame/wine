/*
 * dlls/rsaenh/rsaenh.c
 * RSAENH - RSA encryption for Wine
 *
 * Copyright 2002 TransGaming Technologies (David Hammerton)
 * Copyright 2004 Mike McCormack for CodeWeavers
 * Copyright 2004, 2005 Michael Jung
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public 
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"
#include "wine/library.h"
#include "wine/debug.h"

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "wincrypt.h"
#include "lmcons.h"
#include "handle.h"
#include "implglue.h"
#include "objbase.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

/******************************************************************************
 * CRYPTHASH - hash objects
 */
#define RSAENH_MAGIC_HASH           0x85938417u
#define RSAENH_MAX_HASH_SIZE        104
#define RSAENH_HASHSTATE_IDLE       0
#define RSAENH_HASHSTATE_HASHING    1
#define RSAENH_HASHSTATE_FINISHED   2
typedef struct _RSAENH_TLS1PRF_PARAMS
{
    CRYPT_DATA_BLOB blobLabel;
    CRYPT_DATA_BLOB blobSeed;
} RSAENH_TLS1PRF_PARAMS;

typedef struct tagCRYPTHASH
{
    OBJECTHDR    header;
    ALG_ID       aiAlgid;
    HCRYPTKEY    hKey;
    HCRYPTPROV   hProv;
    DWORD        dwHashSize;
    DWORD        dwState;
    HASH_CONTEXT context;
    BYTE         abHashValue[RSAENH_MAX_HASH_SIZE];
    PHMAC_INFO   pHMACInfo;
    RSAENH_TLS1PRF_PARAMS tpPRFParams;
} CRYPTHASH;

/******************************************************************************
 * CRYPTKEY - key objects
 */
#define RSAENH_MAGIC_KEY           0x73620457u
#define RSAENH_MAX_KEY_SIZE        48
#define RSAENH_MAX_BLOCK_SIZE      24
#define RSAENH_KEYSTATE_IDLE       0
#define RSAENH_KEYSTATE_ENCRYPTING 1
#define RSAENH_KEYSTATE_DECRYPTING 2
#define RSAENH_KEYSTATE_MASTERKEY  3
typedef struct _RSAENH_SCHANNEL_INFO 
{
    SCHANNEL_ALG saEncAlg;
    SCHANNEL_ALG saMACAlg;
    CRYPT_DATA_BLOB blobClientRandom;
    CRYPT_DATA_BLOB blobServerRandom;
} RSAENH_SCHANNEL_INFO;

typedef struct tagCRYPTKEY
{
    OBJECTHDR   header;
    ALG_ID      aiAlgid;
    HCRYPTPROV  hProv;
    DWORD       dwMode;
    DWORD       dwModeBits;
    DWORD       dwPermissions;
    DWORD       dwKeyLen;
    DWORD       dwSaltLen;
    DWORD       dwBlockLen;
    DWORD       dwState;
    KEY_CONTEXT context;    
    BYTE        abKeyValue[RSAENH_MAX_KEY_SIZE];
    BYTE        abInitVector[RSAENH_MAX_BLOCK_SIZE];
    BYTE        abChainVector[RSAENH_MAX_BLOCK_SIZE];
    RSAENH_SCHANNEL_INFO siSChannelInfo;
} CRYPTKEY;

/******************************************************************************
 * KEYCONTAINER - key containers
 */
#define RSAENH_PERSONALITY_BASE        0u
#define RSAENH_PERSONALITY_STRONG      1u
#define RSAENH_PERSONALITY_ENHANCED    2u
#define RSAENH_PERSONALITY_SCHANNEL    3u

#define RSAENH_MAGIC_CONTAINER         0x26384993u
typedef struct tagKEYCONTAINER
{
    OBJECTHDR    header;
    DWORD        dwFlags;
    DWORD        dwPersonality;
    DWORD        dwEnumAlgsCtr;
    DWORD        dwEnumContainersCtr;
    CHAR         szName[MAX_PATH];
    CHAR         szProvName[MAX_PATH];
    HCRYPTKEY    hKeyExchangeKeyPair;
    HCRYPTKEY    hSignatureKeyPair;
} KEYCONTAINER;

/******************************************************************************
 * Some magic constants
 */
#define RSAENH_ENCRYPT                    1
#define RSAENH_DECRYPT                    0    
#define RSAENH_HMAC_DEF_IPAD_CHAR      0x36
#define RSAENH_HMAC_DEF_OPAD_CHAR      0x5c
#define RSAENH_HMAC_DEF_PAD_LEN          64
#define RSAENH_DES_EFFECTIVE_KEYLEN      56
#define RSAENH_DES_STORAGE_KEYLEN        64
#define RSAENH_3DES112_EFFECTIVE_KEYLEN 112
#define RSAENH_3DES112_STORAGE_KEYLEN   128
#define RSAENH_3DES_EFFECTIVE_KEYLEN    168
#define RSAENH_3DES_STORAGE_KEYLEN      192
#define RSAENH_MAGIC_RSA2        0x32415352
#define RSAENH_MAGIC_RSA1        0x31415352
#define RSAENH_PKC_BLOCKTYPE           0x02
#define RSAENH_SSL3_VERSION_MAJOR         3
#define RSAENH_SSL3_VERSION_MINOR         0
#define RSAENH_TLS1_VERSION_MAJOR         3
#define RSAENH_TLS1_VERSION_MINOR         1
#define RSAENH_REGKEY "Software\\Wine\\Crypto\\RSA\\%s"

#define RSAENH_MIN(a,b) ((a)<(b)?(a):(b))
/******************************************************************************
 * aProvEnumAlgsEx - Defines the capabilities of the CSP personalities.
 */
#define RSAENH_MAX_ENUMALGS 20
#define RSAENH_PCT1_SSL2_SSL3_TLS1 (CRYPT_FLAG_PCT1|CRYPT_FLAG_SSL2|CRYPT_FLAG_SSL3|CRYPT_FLAG_TLS1)
PROV_ENUMALGS_EX aProvEnumAlgsEx[4][RSAENH_MAX_ENUMALGS+1] =
{
 {
  {CALG_RC2,       40, 40,   56,0,                    4,"RC2",     24,"RSA Data Security's RC2"},
  {CALG_RC4,       40, 40,   56,0,                    4,"RC4",     24,"RSA Data Security's RC4"},
  {CALG_DES,       56, 56,   56,0,                    4,"DES",     31,"Data Encryption Standard (DES)"},
  {CALG_SHA,      160,160,  160,CRYPT_FLAG_SIGNING,   6,"SHA-1",   30,"Secure Hash Algorithm (SHA-1)"},
  {CALG_MD2,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD2",     23,"Message Digest 2 (MD2)"},
  {CALG_MD4,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD4",     23,"Message Digest 4 (MD4)"},
  {CALG_MD5,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD5",     23,"Message Digest 5 (MD5)"},
  {CALG_SSL3_SHAMD5,288,288,288,0,                   12,"SSL3 SHAMD5",12,"SSL3 SHAMD5"},
  {CALG_MAC,        0,  0,    0,0,                    4,"MAC",     28,"Message Authentication Code"},
  {CALG_RSA_SIGN, 512,384,16384,CRYPT_FLAG_SIGNING|CRYPT_FLAG_IPSEC,9,"RSA_SIGN",14,"RSA Signature"},
  {CALG_RSA_KEYX, 512,384, 1024,CRYPT_FLAG_SIGNING|CRYPT_FLAG_IPSEC,9,"RSA_KEYX",17,"RSA Key Exchange"},
  {CALG_HMAC,       0,  0,    0,0,                    5,"HMAC",    18,"Hugo's MAC (HMAC)"},
  {0,               0,  0,    0,0,                    1,"",         1,""}
 },
 {
  {CALG_RC2,      128, 40,  128,0,                    4,"RC2",     24,"RSA Data Security's RC2"},
  {CALG_RC4,      128, 40,  128,0,                    4,"RC4",     24,"RSA Data Security's RC4"},
  {CALG_DES,       56, 56,   56,0,                    4,"DES",     31,"Data Encryption Standard (DES)"},
  {CALG_3DES_112, 112,112,  112,0,                   13,"3DES TWO KEY",19,"Two Key Triple DES"},
  {CALG_3DES,     168,168,  168,0,                    5,"3DES",    21,"Three Key Triple DES"},
  {CALG_SHA,      160,160,  160,CRYPT_FLAG_SIGNING,   6,"SHA-1",   30,"Secure Hash Algorithm (SHA-1)"},
  {CALG_MD2,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD2",     23,"Message Digest 2 (MD2)"},
  {CALG_MD4,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD4",     23,"Message Digest 4 (MD4)"},
  {CALG_MD5,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD5",     23,"Message Digest 5 (MD5)"},
  {CALG_SSL3_SHAMD5,288,288,288,0,                   12,"SSL3 SHAMD5",12,"SSL3 SHAMD5"},
  {CALG_MAC,        0,  0,    0,0,                    4,"MAC",     28,"Message Authentication Code"},
  {CALG_RSA_SIGN,1024,384,16384,CRYPT_FLAG_SIGNING|CRYPT_FLAG_IPSEC,9,"RSA_SIGN",14,"RSA Signature"},
  {CALG_RSA_KEYX,1024,384,16384,CRYPT_FLAG_SIGNING|CRYPT_FLAG_IPSEC,9,"RSA_KEYX",17,"RSA Key Exchange"},
  {CALG_HMAC,       0,  0,    0,0,                    5,"HMAC",    18,"Hugo's MAC (HMAC)"},
  {0,               0,  0,    0,0,                    1,"",         1,""}
 },
 {
  {CALG_RC2,      128, 40,  128,0,                    4,"RC2",     24,"RSA Data Security's RC2"},
  {CALG_RC4,      128, 40,  128,0,                    4,"RC4",     24,"RSA Data Security's RC4"},
  {CALG_DES,       56, 56,   56,0,                    4,"DES",     31,"Data Encryption Standard (DES)"},
  {CALG_3DES_112, 112,112,  112,0,                   13,"3DES TWO KEY",19,"Two Key Triple DES"},
  {CALG_3DES,     168,168,  168,0,                    5,"3DES",    21,"Three Key Triple DES"},
  {CALG_SHA,      160,160,  160,CRYPT_FLAG_SIGNING,   6,"SHA-1",   30,"Secure Hash Algorithm (SHA-1)"},
  {CALG_MD2,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD2",     23,"Message Digest 2 (MD2)"},
  {CALG_MD4,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD4",     23,"Message Digest 4 (MD4)"},
  {CALG_MD5,      128,128,  128,CRYPT_FLAG_SIGNING,   4,"MD5",     23,"Message Digest 5 (MD5)"},
  {CALG_SSL3_SHAMD5,288,288,288,0,                   12,"SSL3 SHAMD5",12,"SSL3 SHAMD5"},
  {CALG_MAC,        0,  0,    0,0,                    4,"MAC",     28,"Message Authentication Code"},
  {CALG_RSA_SIGN,1024,384,16384,CRYPT_FLAG_SIGNING|CRYPT_FLAG_IPSEC,9,"RSA_SIGN",14,"RSA Signature"},
  {CALG_RSA_KEYX,1024,384,16384,CRYPT_FLAG_SIGNING|CRYPT_FLAG_IPSEC,9,"RSA_KEYX",17,"RSA Key Exchange"},
  {CALG_HMAC,       0,  0,    0,0,                    5,"HMAC",    18,"Hugo's MAC (HMAC)"},
  {0,               0,  0,    0,0,                    1,"",         1,""}
 },
 {
  {CALG_RC2,      128, 40,  128,RSAENH_PCT1_SSL2_SSL3_TLS1, 4,"RC2",        24,"RSA Data Security's RC2"},
  {CALG_RC4,      128, 40,  128,RSAENH_PCT1_SSL2_SSL3_TLS1, 4,"RC4",        24,"RSA Data Security's RC4"},
  {CALG_DES,       56, 56,   56,RSAENH_PCT1_SSL2_SSL3_TLS1, 4,"DES",        31,"Data Encryption Standard (DES)"},
  {CALG_3DES_112, 112,112,  112,RSAENH_PCT1_SSL2_SSL3_TLS1,13,"3DES TWO KEY",19,"Two Key Triple DES"},
  {CALG_3DES,     168,168,  168,RSAENH_PCT1_SSL2_SSL3_TLS1, 5,"3DES",       21,"Three Key Triple DES"},
  {CALG_SHA,160,160,160,CRYPT_FLAG_SIGNING|RSAENH_PCT1_SSL2_SSL3_TLS1,6,"SHA-1",30,"Secure Hash Algorithm (SHA-1)"},
  {CALG_MD5,128,128,128,CRYPT_FLAG_SIGNING|RSAENH_PCT1_SSL2_SSL3_TLS1,4,"MD5",23,"Message Digest 5 (MD5)"},
  {CALG_SSL3_SHAMD5,288,288,288,0,                         12,"SSL3 SHAMD5",12,"SSL3 SHAMD5"},
  {CALG_MAC,        0,  0,    0,0,                          4,"MAC",        28,"Message Authentication Code"},
  {CALG_RSA_SIGN,1024,384,16384,CRYPT_FLAG_SIGNING|RSAENH_PCT1_SSL2_SSL3_TLS1,9,"RSA_SIGN",14,"RSA Signature"},
  {CALG_RSA_KEYX,1024,384,16384,CRYPT_FLAG_SIGNING|RSAENH_PCT1_SSL2_SSL3_TLS1,9,"RSA_KEYX",17,"RSA Key Exchange"},
  {CALG_HMAC,       0,  0,    0,0,                          5,"HMAC",       18,"Hugo's MAC (HMAC)"},
  {CALG_PCT1_MASTER,128,128,128,CRYPT_FLAG_PCT1,           12,"PCT1 MASTER",12,"PCT1 Master"},
  {CALG_SSL2_MASTER,40,40,  192,CRYPT_FLAG_SSL2,           12,"SSL2 MASTER",12,"SSL2 Master"},
  {CALG_SSL3_MASTER,384,384,384,CRYPT_FLAG_SSL3,           12,"SSL3 MASTER",12,"SSL3 Master"},
  {CALG_TLS1_MASTER,384,384,384,CRYPT_FLAG_TLS1,           12,"TLS1 MASTER",12,"TLS1 Master"},
  {CALG_SCHANNEL_MASTER_HASH,0,0,-1,0,                     16,"SCH MASTER HASH",21,"SChannel Master Hash"},
  {CALG_SCHANNEL_MAC_KEY,0,0,-1,0,                         12,"SCH MAC KEY",17,"SChannel MAC Key"},
  {CALG_SCHANNEL_ENC_KEY,0,0,-1,0,                         12,"SCH ENC KEY",24,"SChannel Encryption Key"},
  {CALG_TLS1PRF,    0,  0,   -1,0,                          9,"TLS1 PRF",   28,"TLS1 Pseudo Random Function"},
  {0,               0,  0,    0,0,                          1,"",            1,""}
 }
};

/******************************************************************************
 * API forward declarations
 */
BOOL WINAPI 
RSAENH_CPGetKeyParam(
    HCRYPTPROV hProv, 
    HCRYPTKEY hKey, 
    DWORD dwParam, 
    BYTE *pbData, 
    DWORD *pdwDataLen, 
    DWORD dwFlags
);

BOOL WINAPI 
RSAENH_CPEncrypt(
    HCRYPTPROV hProv, 
    HCRYPTKEY hKey, 
    HCRYPTHASH hHash, 
    BOOL Final, 
    DWORD dwFlags, 
    BYTE *pbData,
    DWORD *pdwDataLen, 
    DWORD dwBufLen
);

BOOL WINAPI 
RSAENH_CPCreateHash(
    HCRYPTPROV hProv, 
    ALG_ID Algid, 
    HCRYPTKEY hKey, 
    DWORD dwFlags, 
    HCRYPTHASH *phHash
);

BOOL WINAPI 
RSAENH_CPSetHashParam(
    HCRYPTPROV hProv, 
    HCRYPTHASH hHash, 
    DWORD dwParam, 
    BYTE *pbData, DWORD dwFlags
);

BOOL WINAPI 
RSAENH_CPGetHashParam(
    HCRYPTPROV hProv, 
    HCRYPTHASH hHash, 
    DWORD dwParam, 
    BYTE *pbData, 
    DWORD *pdwDataLen, 
    DWORD dwFlags
);

BOOL WINAPI 
RSAENH_CPDestroyHash(
    HCRYPTPROV hProv, 
    HCRYPTHASH hHash
);

BOOL WINAPI 
RSAENH_CPExportKey(
    HCRYPTPROV hProv, 
    HCRYPTKEY hKey, 
    HCRYPTKEY hPubKey, 
    DWORD dwBlobType, 
    DWORD dwFlags, 
    BYTE *pbData, 
    DWORD *pdwDataLen
);

BOOL WINAPI 
RSAENH_CPImportKey(
    HCRYPTPROV hProv, 
    CONST BYTE *pbData, 
    DWORD dwDataLen, 
    HCRYPTKEY hPubKey, 
    DWORD dwFlags, 
    HCRYPTKEY *phKey
);

BOOL WINAPI 
RSAENH_CPHashData(
    HCRYPTPROV hProv, 
    HCRYPTHASH hHash, 
    CONST BYTE *pbData, 
    DWORD dwDataLen, 
    DWORD dwFlags
);

/******************************************************************************
 * CSP's handle table (used by all acquired key containers)
 */
static HANDLETABLE handle_table;

/******************************************************************************
 * DllMain (RSAENH.@)
 *
 * Initializes and destroys the handle table for the CSP's handles.
 */
int WINAPI DllMain(HINSTANCE hInstance, DWORD fdwReason, PVOID pvReserved)
{
    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hInstance);
            init_handle_table(&handle_table);
            break;

        case DLL_PROCESS_DETACH:
            destroy_handle_table(&handle_table);
            break;
    }
    return 1;
}

/******************************************************************************
 * copy_param [Internal]
 *
 * Helper function that supports the standard WINAPI protocol for querying data
 * of dynamic size.
 *
 * PARAMS
 *  pbBuffer      [O]   Buffer where the queried parameter is copied to, if it is large enough.
 *                      May be NUL if the required buffer size is to be queried only.
 *  pdwBufferSize [I/O] In: Size of the buffer at pbBuffer
 *                      Out: Size of parameter pbParam
 *  pbParam       [I]   Parameter value.
 *  dwParamSize   [I]   Size of pbParam
 *
 * RETURN
 *  Success: TRUE (pbParam was copied into pbBuffer or pbBuffer is NULL)
 *  Failure: FALSE (pbBuffer is not large enough to hold pbParam). Last error: ERROR_MORE_DATA
 */
static inline BOOL copy_param(
    BYTE *pbBuffer, DWORD *pdwBufferSize, CONST BYTE *pbParam, DWORD dwParamSize) 
{
    if (pbBuffer) 
    {
        if (dwParamSize > *pdwBufferSize) 
        {
            SetLastError(ERROR_MORE_DATA);
            *pdwBufferSize = dwParamSize;
            return FALSE;
        }
        memcpy(pbBuffer, pbParam, dwParamSize);
    }
    *pdwBufferSize = dwParamSize;
    return TRUE;
}

/******************************************************************************
 * get_algid_info [Internal]
 *
 * Query CSP capabilities for a given crypto algorithm.
 * 
 * PARAMS
 *  hProv [I] Handle to a key container of the CSP whose capabilities are to be queried.
 *  algid [I] Identifier of the crypto algorithm about which information is requested.
 *
 * RETURNS
 *  Success: Pointer to a PROV_ENUMALGS_EX struct containing information about the crypto algorithm.
 *  Failure: NULL (algid not supported)
 */
static inline const PROV_ENUMALGS_EX* get_algid_info(HCRYPTPROV hProv, ALG_ID algid) {
    PROV_ENUMALGS_EX *iterator;
    KEYCONTAINER *pKeyContainer;

    if (!lookup_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER, (OBJECTHDR**)&pKeyContainer)) {
        SetLastError(NTE_BAD_UID);
        return NULL;
    }

    for (iterator = aProvEnumAlgsEx[pKeyContainer->dwPersonality]; iterator->aiAlgid; iterator++) {
        if (iterator->aiAlgid == algid) return iterator;
    }

    SetLastError(NTE_BAD_ALGID);
    return NULL;
}

/******************************************************************************
 * copy_data_blob [Internal] 
 *
 * deeply copies a DATA_BLOB
 *
 * PARAMS
 *  dst [O] That's where the blob will be copied to
 *  src [I] Source blob
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE (GetLastError() == NTE_NO_MEMORY
 *
 * NOTES
 *  Use free_data_blob to release resources occupied by copy_data_blob.
 */
static inline BOOL copy_data_blob(PCRYPT_DATA_BLOB dst, CONST PCRYPT_DATA_BLOB src) {
    dst->pbData = HeapAlloc(GetProcessHeap(), 0, src->cbData);
    if (!dst->pbData) {
        SetLastError(NTE_NO_MEMORY);
        return FALSE;
    }    
    dst->cbData = src->cbData;
    memcpy(dst->pbData, src->pbData, src->cbData);
    return TRUE;
}

/******************************************************************************
 * concat_data_blobs [Internal]
 *
 * Concatenates two blobs
 *
 * PARAMS
 *  dst  [O] The new blob will be copied here
 *  src1 [I] Prefix blob
 *  src2 [I] Appendix blob
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE (GetLastError() == NTE_NO_MEMORY)
 *
 * NOTES
 *  Release resources occupied by concat_data_blobs with free_data_blobs
 */
static inline BOOL concat_data_blobs(PCRYPT_DATA_BLOB dst, CONST PCRYPT_DATA_BLOB src1, 
                                     CONST PCRYPT_DATA_BLOB src2) 
{
    dst->cbData = src1->cbData + src2->cbData;
    dst->pbData = HeapAlloc(GetProcessHeap(), 0, dst->cbData);
    if (!dst->pbData) {
        SetLastError(NTE_NO_MEMORY);
        return FALSE;
    }
    memcpy(dst->pbData, src1->pbData, src1->cbData);
    memcpy(dst->pbData + src1->cbData, src2->pbData, src2->cbData);
    return TRUE;
}

/******************************************************************************
 * free_data_blob [Internal]
 *
 * releases resource occupied by a dynamically allocated CRYPT_DATA_BLOB
 * 
 * PARAMS
 *  pBlob [I] Heap space occupied by pBlob->pbData is released
 */
static inline void free_data_blob(PCRYPT_DATA_BLOB pBlob) {
    HeapFree(GetProcessHeap(), 0, pBlob->pbData);
}

/******************************************************************************
 * init_data_blob [Internal]
 */
static inline void init_data_blob(PCRYPT_DATA_BLOB pBlob) {
    pBlob->pbData = NULL;
    pBlob->cbData = 0;
}

/******************************************************************************
 * free_hmac_info [Internal]
 *
 * Deeply free an HMAC_INFO struct.
 *
 * PARAMS
 *  hmac_info [I] Pointer to the HMAC_INFO struct to be freed.
 *
 * NOTES
 *  See Internet RFC 2104 for details on the HMAC algorithm.
 */
static inline void free_hmac_info(PHMAC_INFO hmac_info) {
    if (!hmac_info) return;
    HeapFree(GetProcessHeap(), 0, hmac_info->pbInnerString);
    HeapFree(GetProcessHeap(), 0, hmac_info->pbOuterString);
    HeapFree(GetProcessHeap(), 0, hmac_info);
}

/******************************************************************************
 * copy_hmac_info [Internal]
 *
 * Deeply copy an HMAC_INFO struct
 *
 * PARAMS
 *  dst [O] Pointer to a location where the pointer to the HMAC_INFO copy will be stored.
 *  src [I] Pointer to the HMAC_INFO struct to be copied.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 *
 * NOTES
 *  See Internet RFC 2104 for details on the HMAC algorithm.
 */
static BOOL copy_hmac_info(PHMAC_INFO *dst, PHMAC_INFO src) {
    if (!src) return FALSE;
    *dst = HeapAlloc(GetProcessHeap(), 0, sizeof(HMAC_INFO));
    if (!*dst) return FALSE;
    memcpy(*dst, src, sizeof(HMAC_INFO));
    (*dst)->pbInnerString = NULL;
    (*dst)->pbOuterString = NULL;
    if ((*dst)->cbInnerString == 0) (*dst)->cbInnerString = RSAENH_HMAC_DEF_PAD_LEN;
    (*dst)->pbInnerString = HeapAlloc(GetProcessHeap(), 0, (*dst)->cbInnerString);
    if (!(*dst)->pbInnerString) {
        free_hmac_info(*dst);
        return FALSE;
    }
    if (src->cbInnerString) 
        memcpy((*dst)->pbInnerString, src->pbInnerString, src->cbInnerString);
    else 
        memset((*dst)->pbInnerString, RSAENH_HMAC_DEF_IPAD_CHAR, RSAENH_HMAC_DEF_PAD_LEN);
    if ((*dst)->cbOuterString == 0) (*dst)->cbOuterString = RSAENH_HMAC_DEF_PAD_LEN;
    (*dst)->pbOuterString = HeapAlloc(GetProcessHeap(), 0, (*dst)->cbOuterString);
    if (!(*dst)->pbOuterString) {
        free_hmac_info(*dst);
        return FALSE;
    }
    if (src->cbOuterString) 
        memcpy((*dst)->pbOuterString, src->pbOuterString, src->cbOuterString);
    else 
        memset((*dst)->pbOuterString, RSAENH_HMAC_DEF_OPAD_CHAR, RSAENH_HMAC_DEF_PAD_LEN);
    return TRUE;
}

/******************************************************************************
 * destroy_hash [Internal]
 *
 * Destructor for hash objects
 *
 * PARAMS
 *  pCryptHash [I] Pointer to the hash object to be destroyed. 
 *                 Will be invalid after function returns!
 */
static void destroy_hash(OBJECTHDR *pObject)
{
    CRYPTHASH *pCryptHash = (CRYPTHASH*)pObject;
        
    free_hmac_info(pCryptHash->pHMACInfo);
    free_data_blob(&pCryptHash->tpPRFParams.blobLabel);
    free_data_blob(&pCryptHash->tpPRFParams.blobSeed);
    HeapFree(GetProcessHeap(), 0, pCryptHash);
}

/******************************************************************************
 * init_hash [Internal]
 *
 * Initialize (or reset) a hash object
 *
 * PARAMS
 *  pCryptHash    [I] The hash object to be initialized.
 */
static inline BOOL init_hash(CRYPTHASH *pCryptHash) {
    DWORD dwLen;
        
    switch (pCryptHash->aiAlgid) 
    {
        case CALG_HMAC:
            if (pCryptHash->pHMACInfo) { 
                const PROV_ENUMALGS_EX *pAlgInfo;
                
                pAlgInfo = get_algid_info(pCryptHash->hProv, pCryptHash->pHMACInfo->HashAlgid);
                if (!pAlgInfo) return FALSE;
                pCryptHash->dwHashSize = pAlgInfo->dwDefaultLen >> 3;
                init_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context);
                update_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context,
                                 pCryptHash->pHMACInfo->pbInnerString, 
                                 pCryptHash->pHMACInfo->cbInnerString);
            }
            return TRUE;
            
        case CALG_MAC:
            dwLen = sizeof(DWORD);
            RSAENH_CPGetKeyParam(pCryptHash->hProv, pCryptHash->hKey, KP_BLOCKLEN, 
                                 (BYTE*)&pCryptHash->dwHashSize, &dwLen, 0);
            pCryptHash->dwHashSize >>= 3;
            return TRUE;

        default:
            return init_hash_impl(pCryptHash->aiAlgid, &pCryptHash->context);
    }
}

/******************************************************************************
 * update_hash [Internal]
 *
 * Hashes the given data and updates the hash object's state accordingly
 *
 * PARAMS
 *  pCryptHash [I] Hash object to be updated.
 *  pbData     [I] Pointer to data stream to be hashed.
 *  dwDataLen  [I] Length of data stream.
 */
static inline void update_hash(CRYPTHASH *pCryptHash, CONST BYTE *pbData, DWORD dwDataLen) {
    BYTE *pbTemp;

    switch (pCryptHash->aiAlgid)
    {
        case CALG_HMAC:
            if (pCryptHash->pHMACInfo) 
                update_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context, 
                                 pbData, dwDataLen);
            break;

        case CALG_MAC:
            pbTemp = HeapAlloc(GetProcessHeap(), 0, dwDataLen);
            if (!pbTemp) return;
            memcpy(pbTemp, pbData, dwDataLen);
            RSAENH_CPEncrypt(pCryptHash->hProv, pCryptHash->hKey, (HCRYPTHASH)NULL, FALSE, 0, 
                             pbTemp, &dwDataLen, dwDataLen);
            HeapFree(GetProcessHeap(), 0, pbTemp);
            break;

        default:
            update_hash_impl(pCryptHash->aiAlgid, &pCryptHash->context, pbData, dwDataLen);
    }
}

/******************************************************************************
 * finalize_hash [Internal]
 *
 * Finalizes the hash, after all data has been hashed with update_hash.
 * No additional data can be hashed afterwards until the hash gets initialized again.
 *
 * PARAMS
 *  pCryptHash [I] Hash object to be finalized.
 */
static inline void finalize_hash(CRYPTHASH *pCryptHash) {
    DWORD dwDataLen;
        
    switch (pCryptHash->aiAlgid)
    {
        case CALG_HMAC:
            if (pCryptHash->pHMACInfo) {
                BYTE abHashValue[RSAENH_MAX_HASH_SIZE];

                finalize_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context, 
                                   pCryptHash->abHashValue);
                memcpy(abHashValue, pCryptHash->abHashValue, pCryptHash->dwHashSize);
                init_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context);
                update_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context,
                                 pCryptHash->pHMACInfo->pbOuterString, 
                                 pCryptHash->pHMACInfo->cbOuterString);
                update_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context,
                                 abHashValue, pCryptHash->dwHashSize);
                finalize_hash_impl(pCryptHash->pHMACInfo->HashAlgid, &pCryptHash->context,
                                   pCryptHash->abHashValue);
            } 
            break;

        case CALG_MAC:
            dwDataLen = 0;
            RSAENH_CPEncrypt(pCryptHash->hProv, pCryptHash->hKey, (HCRYPTHASH)NULL, TRUE, 0, 
                             pCryptHash->abHashValue, &dwDataLen, pCryptHash->dwHashSize);
            break;

        default:
            finalize_hash_impl(pCryptHash->aiAlgid, &pCryptHash->context, pCryptHash->abHashValue);
    }
}

/******************************************************************************
 * destroy_key [Internal]
 *
 * Destructor for key objects
 *
 * PARAMS
 *  pCryptKey [I] Pointer to the key object to be destroyed. 
 *                Will be invalid after function returns!
 */
static void destroy_key(OBJECTHDR *pObject)
{
    CRYPTKEY *pCryptKey = (CRYPTKEY*)pObject;
        
    free_key_impl(pCryptKey->aiAlgid, &pCryptKey->context);
    free_data_blob(&pCryptKey->siSChannelInfo.blobClientRandom);
    free_data_blob(&pCryptKey->siSChannelInfo.blobServerRandom);
    HeapFree(GetProcessHeap(), 0, pCryptKey);
}

/******************************************************************************
 * setup_key [Internal]
 *
 * Initialize (or reset) a key object
 *
 * PARAMS
 *  pCryptKey    [I] The key object to be initialized.
 */
static inline void setup_key(CRYPTKEY *pCryptKey) {
    pCryptKey->dwState = RSAENH_KEYSTATE_IDLE;
    memcpy(pCryptKey->abChainVector, pCryptKey->abInitVector, sizeof(pCryptKey->abChainVector));
    setup_key_impl(pCryptKey->aiAlgid, &pCryptKey->context, pCryptKey->dwKeyLen, 
                   pCryptKey->dwSaltLen, pCryptKey->abKeyValue);
}

/******************************************************************************
 * new_key [Internal]
 *
 * Creates a new key object without assigning the actual binary key value. 
 * This is done by CPDeriveKey, CPGenKey or CPImportKey, which call this function.
 *
 * PARAMS
 *  hProv      [I] Handle to the provider to which the created key will belong.
 *  aiAlgid    [I] The new key shall use the crypto algorithm idenfied by aiAlgid.
 *  dwFlags    [I] Upper 16 bits give the key length.
 *                 Lower 16 bits: CRYPT_CREATE_SALT, CRYPT_NO_SALT
 *  ppCryptKey [O] Pointer to the created key
 *
 * RETURNS
 *  Success: Handle to the created key.
 *  Failure: INVALID_HANDLE_VALUE
 */
static HCRYPTKEY new_key(HCRYPTPROV hProv, ALG_ID aiAlgid, DWORD dwFlags, CRYPTKEY **ppCryptKey)
{
    HCRYPTKEY hCryptKey;
    CRYPTKEY *pCryptKey;
    DWORD dwKeyLen = HIWORD(dwFlags);
    const PROV_ENUMALGS_EX *peaAlgidInfo;

    *ppCryptKey = NULL;
    
    /* 
     * Retrieve the CSP's capabilities for the given ALG_ID value
     */
    peaAlgidInfo = get_algid_info(hProv, aiAlgid);
    if (!peaAlgidInfo) return (HCRYPTKEY)INVALID_HANDLE_VALUE;

    /*
     * Assume the default key length, if none is specified explicitly
     */
    if (dwKeyLen == 0) dwKeyLen = peaAlgidInfo->dwDefaultLen;
    
    /*
     * Check if the requested key length is supported by the current CSP.
     * Adjust key length's for DES algorithms.
     */
    switch (aiAlgid) {
        case CALG_DES:
            if (dwKeyLen == RSAENH_DES_EFFECTIVE_KEYLEN) {
                dwKeyLen = RSAENH_DES_STORAGE_KEYLEN;
            }
            if (dwKeyLen != RSAENH_DES_STORAGE_KEYLEN) {
                SetLastError(NTE_BAD_FLAGS);
                return (HCRYPTKEY)INVALID_HANDLE_VALUE;
            }
            break;

        case CALG_3DES_112:
            if (dwKeyLen == RSAENH_3DES112_EFFECTIVE_KEYLEN) {
                dwKeyLen = RSAENH_3DES112_STORAGE_KEYLEN;
            }
            if (dwKeyLen != RSAENH_3DES112_STORAGE_KEYLEN) {
                SetLastError(NTE_BAD_FLAGS);
                return (HCRYPTKEY)INVALID_HANDLE_VALUE;
            }
            break;

        case CALG_3DES:
            if (dwKeyLen == RSAENH_3DES_EFFECTIVE_KEYLEN) {
                dwKeyLen = RSAENH_3DES_STORAGE_KEYLEN;
            }
            if (dwKeyLen != RSAENH_3DES_STORAGE_KEYLEN) {
                SetLastError(NTE_BAD_FLAGS);
                return (HCRYPTKEY)INVALID_HANDLE_VALUE;
            }
            break;
        
        default:
            if (dwKeyLen % 8 || 
                dwKeyLen > peaAlgidInfo->dwMaxLen || 
                dwKeyLen < peaAlgidInfo->dwMinLen) 
            {
                SetLastError(NTE_BAD_FLAGS);
                return (HCRYPTKEY)INVALID_HANDLE_VALUE;
            }
    }

    hCryptKey = (HCRYPTKEY)new_object(&handle_table, sizeof(CRYPTKEY), RSAENH_MAGIC_KEY, 
                                      destroy_key, (OBJECTHDR**)&pCryptKey);
    if (hCryptKey != (HCRYPTKEY)INVALID_HANDLE_VALUE)
    {
        pCryptKey->aiAlgid = aiAlgid;
        pCryptKey->hProv = hProv;
        pCryptKey->dwModeBits = 0;
        pCryptKey->dwPermissions = CRYPT_ENCRYPT | CRYPT_DECRYPT | CRYPT_READ | CRYPT_WRITE | 
                                   CRYPT_MAC;
        pCryptKey->dwKeyLen = dwKeyLen >> 3;
        if ((dwFlags & CRYPT_CREATE_SALT) || (dwKeyLen == 40 && !(dwFlags & CRYPT_NO_SALT))) 
            pCryptKey->dwSaltLen = 16 /*FIXME*/ - pCryptKey->dwKeyLen;
        else
            pCryptKey->dwSaltLen = 0;
        memset(pCryptKey->abKeyValue, 0, sizeof(pCryptKey->abKeyValue));
        memset(pCryptKey->abInitVector, 0, sizeof(pCryptKey->abInitVector));
        init_data_blob(&pCryptKey->siSChannelInfo.blobClientRandom);
        init_data_blob(&pCryptKey->siSChannelInfo.blobServerRandom);
            
        switch(aiAlgid)
        {
            case CALG_PCT1_MASTER:
            case CALG_SSL2_MASTER:
            case CALG_SSL3_MASTER:
            case CALG_TLS1_MASTER:
            case CALG_RC4:
                pCryptKey->dwBlockLen = 0;
                pCryptKey->dwMode = 0;
                break;

            case CALG_RC2:
            case CALG_DES:
            case CALG_3DES_112:
            case CALG_3DES:
                pCryptKey->dwBlockLen = 8;
                pCryptKey->dwMode = CRYPT_MODE_CBC;
                break;

            case CALG_RSA_KEYX:
            case CALG_RSA_SIGN:
                pCryptKey->dwBlockLen = dwKeyLen >> 3;
                pCryptKey->dwMode = 0;
                break;
        }

        *ppCryptKey = pCryptKey;
    }

    return hCryptKey;
}

/******************************************************************************
 * destroy_key_container [Internal]
 *
 * Destructor for key containers.
 * 
 * PARAMS
 *  pObjectHdr [I] Pointer to the key container to be destroyed.
 */
static void destroy_key_container(OBJECTHDR *pObjectHdr)
{
    KEYCONTAINER *pKeyContainer = (KEYCONTAINER*)pObjectHdr;
    DATA_BLOB blobIn, blobOut;
    CRYPTKEY *pKey;
    CHAR szRSABase[MAX_PATH];
    HKEY hKey, hRootKey;
    DWORD dwLen;
    BYTE *pbKey;

    if (!(pKeyContainer->dwFlags & CRYPT_VERIFYCONTEXT)) {
        /* On WinXP, persistent keys are stored in a file located at: 
         * $AppData$\\Microsoft\\Crypto\\RSA\\$SID$\\some_hex_string 
         */
        sprintf(szRSABase, RSAENH_REGKEY, pKeyContainer->szName);

        if (pKeyContainer->dwFlags & CRYPT_MACHINE_KEYSET) {
            hRootKey = HKEY_LOCAL_MACHINE;
        } else {
            hRootKey = HKEY_CURRENT_USER;
        }
        
        /* @@ Wine registry key: HKLM\Software\Wine\Crypto\RSA */
        /* @@ Wine registry key: HKCU\Software\Wine\Crypto\RSA */
        if (RegCreateKeyExA(hRootKey, szRSABase, 0, NULL, REG_OPTION_NON_VOLATILE, 
                            KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
        {
            if (lookup_handle(&handle_table, pKeyContainer->hKeyExchangeKeyPair, RSAENH_MAGIC_KEY, 
                              (OBJECTHDR**)&pKey))
            {
                if (RSAENH_CPExportKey(pKey->hProv, pKeyContainer->hKeyExchangeKeyPair, 0, 
                                       PRIVATEKEYBLOB, 0, 0, &dwLen)) 
                {
                    pbKey = HeapAlloc(GetProcessHeap(), 0, dwLen);
                    if (pbKey) 
                    {
                        if (RSAENH_CPExportKey(pKey->hProv, pKeyContainer->hKeyExchangeKeyPair, 0,
                                               PRIVATEKEYBLOB, 0, pbKey, &dwLen))
                        {
                            blobIn.pbData = pbKey;
                            blobIn.cbData = dwLen;
                                    
                            if (CryptProtectData(&blobIn, NULL, NULL, NULL, NULL, 
                                 (pKeyContainer->dwFlags & CRYPT_MACHINE_KEYSET) ? 
                                   CRYPTPROTECT_LOCAL_MACHINE : 0, 
                                 &blobOut)) 
                            {
                                RegSetValueExA(hKey, "KeyExchangeKeyPair", 0, REG_BINARY,
                                               blobOut.pbData, blobOut.cbData);
                                HeapFree(GetProcessHeap(), 0, blobOut.pbData);
                            }
                        }
                        HeapFree(GetProcessHeap(), 0, pbKey);
                    }
                }
                release_handle(&handle_table, (unsigned int)pKeyContainer->hKeyExchangeKeyPair, 
                               RSAENH_MAGIC_KEY);
            }

            if (lookup_handle(&handle_table, pKeyContainer->hSignatureKeyPair, RSAENH_MAGIC_KEY, 
                              (OBJECTHDR**)&pKey))
            {
                if (RSAENH_CPExportKey(pKey->hProv, pKeyContainer->hSignatureKeyPair, 0, 
                                       PRIVATEKEYBLOB, 0, 0, &dwLen)) 
                {
                    pbKey = HeapAlloc(GetProcessHeap(), 0, dwLen);
                    if (pbKey) 
                    {
                        if (RSAENH_CPExportKey(pKey->hProv, pKeyContainer->hSignatureKeyPair, 0, 
                                               PRIVATEKEYBLOB, 0, pbKey, &dwLen))
                        {
                            blobIn.pbData = pbKey;
                            blobIn.cbData = dwLen;
                                    
                            if (CryptProtectData(&blobIn, NULL, NULL, NULL, NULL, 
                                 (pKeyContainer->dwFlags & CRYPT_MACHINE_KEYSET) ? 
                                   CRYPTPROTECT_LOCAL_MACHINE : 0, 
                                 &blobOut)) 
                            {
                                RegSetValueExA(hKey, "SignatureKeyPair", 0, REG_BINARY, 
                                               blobOut.pbData, blobOut.cbData);
                                HeapFree(GetProcessHeap(), 0, blobOut.pbData);
                            }
                        }
                        HeapFree(GetProcessHeap(), 0, pbKey);
                    }
                }
                release_handle(&handle_table, (unsigned int)pKeyContainer->hSignatureKeyPair, 
                               RSAENH_MAGIC_KEY);
            }
        
            RegCloseKey(hKey);
        }
    }
    
    HeapFree( GetProcessHeap(), 0, pKeyContainer );
}

/******************************************************************************
 * new_key_container [Internal]
 *
 * Create a new key container. The personality (RSA Base, Strong or Enhanced CP) 
 * of the CSP is determined via the pVTable->pszProvName string.
 *
 * PARAMS
 *  pszContainerName [I] Name of the key container.
 *  pVTable          [I] Callback functions and context info provided by the OS
 *
 * RETURNS
 *  Success: Handle to the new key container.
 *  Failure: INVALID_HANDLE_VALUE
 */
static HCRYPTPROV new_key_container(PCCH pszContainerName, DWORD dwFlags, PVTableProvStruc pVTable)
{
    KEYCONTAINER *pKeyContainer;
    HCRYPTPROV hKeyContainer;

    hKeyContainer = (HCRYPTPROV)new_object(&handle_table, sizeof(KEYCONTAINER), RSAENH_MAGIC_CONTAINER,
                                           destroy_key_container, (OBJECTHDR**)&pKeyContainer);
    if (hKeyContainer != (HCRYPTPROV)INVALID_HANDLE_VALUE)
    {
        lstrcpynA(pKeyContainer->szName, pszContainerName, MAX_PATH);
        pKeyContainer->dwFlags = dwFlags;
        pKeyContainer->dwEnumAlgsCtr = 0;
        pKeyContainer->hKeyExchangeKeyPair = (HCRYPTKEY)INVALID_HANDLE_VALUE;
        pKeyContainer->hSignatureKeyPair = (HCRYPTKEY)INVALID_HANDLE_VALUE;
        if (pVTable && pVTable->pszProvName) {
            lstrcpynA(pKeyContainer->szProvName, pVTable->pszProvName, MAX_PATH);
            if (!strcmp(pVTable->pszProvName, MS_DEF_PROV_A)) {
                pKeyContainer->dwPersonality = RSAENH_PERSONALITY_BASE;
            } else if (!strcmp(pVTable->pszProvName, MS_ENHANCED_PROV_A)) {
                pKeyContainer->dwPersonality = RSAENH_PERSONALITY_ENHANCED;
            } else if (!strcmp(pVTable->pszProvName, MS_DEF_RSA_SCHANNEL_PROV_A)) { 
                pKeyContainer->dwPersonality = RSAENH_PERSONALITY_SCHANNEL;
            } else {
                pKeyContainer->dwPersonality = RSAENH_PERSONALITY_STRONG;
            }
        }

        /* The new key container has to be inserted into the CSP immediately 
         * after creation to be available for CPGetProvParam's PP_ENUMCONTAINERS. */
        if (!(dwFlags & CRYPT_VERIFYCONTEXT)) {
            CHAR szRSABase[MAX_PATH];
            HKEY hRootKey, hKey;

            sprintf(szRSABase, RSAENH_REGKEY, pKeyContainer->szName);

            if (pKeyContainer->dwFlags & CRYPT_MACHINE_KEYSET) {
                hRootKey = HKEY_LOCAL_MACHINE;
            } else {
                hRootKey = HKEY_CURRENT_USER;
            }

            /* @@ Wine registry key: HKLM\Software\Wine\Crypto\RSA */
            /* @@ Wine registry key: HKCU\Software\Wine\Crypto\RSA */
            RegCreateKeyA(hRootKey, szRSABase, &hKey);
            RegCloseKey(hKey);
        }
    }

    return hKeyContainer;
}

/******************************************************************************
 * read_key_container [Internal]
 *
 * Tries to read the persistent state of the key container (mainly the signature
 * and key exchange private keys) given by pszContainerName.
 *
 * PARAMS
 *  pszContainerName [I] Name of the key container to read from the registry
 *  pVTable          [I] Pointer to context data provided by the operating system
 *
 * RETURNS
 *  Success: Handle to the key container read from the registry
 *  Failure: INVALID_HANDLE_VALUE
 */
static HCRYPTPROV read_key_container(PCHAR pszContainerName, DWORD dwFlags, PVTableProvStruc pVTable)
{
    CHAR szRSABase[MAX_PATH];
    BYTE *pbKey;
    HKEY hKey, hRootKey;
    DWORD dwValueType, dwLen;
    KEYCONTAINER *pKeyContainer;
    HCRYPTPROV hKeyContainer;
    DATA_BLOB blobIn, blobOut;
    
    sprintf(szRSABase, RSAENH_REGKEY, pszContainerName);

    if (dwFlags & CRYPT_MACHINE_KEYSET) {
        hRootKey = HKEY_LOCAL_MACHINE;
    } else {
        hRootKey = HKEY_CURRENT_USER;
    }

    /* @@ Wine registry key: HKLM\Software\Wine\Crypto\RSA */
    /* @@ Wine registry key: HKCU\Software\Wine\Crypto\RSA */
    if (RegOpenKeyExA(hRootKey, szRSABase, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        SetLastError(NTE_BAD_KEYSET);
        return (HCRYPTPROV)INVALID_HANDLE_VALUE;
    }

    hKeyContainer = new_key_container(pszContainerName, dwFlags, pVTable);
    if (hKeyContainer != (HCRYPTPROV)INVALID_HANDLE_VALUE)
    {
        if (!lookup_handle(&handle_table, hKeyContainer, RSAENH_MAGIC_CONTAINER, 
                           (OBJECTHDR**)&pKeyContainer))
            return (HCRYPTPROV)INVALID_HANDLE_VALUE;
    
        if (RegQueryValueExA(hKey, "KeyExchangeKeyPair", 0, &dwValueType, NULL, &dwLen) == 
            ERROR_SUCCESS) 
        {
            pbKey = HeapAlloc(GetProcessHeap(), 0, dwLen);
            if (pbKey) 
            {
                if (RegQueryValueExA(hKey, "KeyExchangeKeyPair", 0, &dwValueType, pbKey, &dwLen) ==
                    ERROR_SUCCESS)
                {
                    blobIn.pbData = pbKey;
                    blobIn.cbData = dwLen;

                    if (CryptUnprotectData(&blobIn, NULL, NULL, NULL, NULL, 
                         (dwFlags & CRYPT_MACHINE_KEYSET) ? CRYPTPROTECT_LOCAL_MACHINE : 0, &blobOut))
                    {
                        RSAENH_CPImportKey(hKeyContainer, blobOut.pbData, blobOut.cbData, 0, 0,
                                           &pKeyContainer->hKeyExchangeKeyPair);
                        HeapFree(GetProcessHeap(), 0, blobOut.pbData);
                    }
                }
                HeapFree(GetProcessHeap(), 0, pbKey);
            }
        }

        if (RegQueryValueExA(hKey, "SignatureKeyPair", 0, &dwValueType, NULL, &dwLen) == 
            ERROR_SUCCESS) 
        {
            pbKey = HeapAlloc(GetProcessHeap(), 0, dwLen);
            if (pbKey) 
            {
                if (RegQueryValueExA(hKey, "SignatureKeyPair", 0, &dwValueType, pbKey, &dwLen) == 
                    ERROR_SUCCESS)
                {
                    blobIn.pbData = pbKey;
                    blobIn.cbData = dwLen;

                    if (CryptUnprotectData(&blobIn, NULL, NULL, NULL, NULL, 
                         (dwFlags & CRYPT_MACHINE_KEYSET) ? CRYPTPROTECT_LOCAL_MACHINE : 0, &blobOut))
                    {
                        RSAENH_CPImportKey(hKeyContainer, blobOut.pbData, blobOut.cbData, 0, 0,
                                           &pKeyContainer->hSignatureKeyPair);
                        HeapFree(GetProcessHeap(), 0, blobOut.pbData);
                    }
                }
                HeapFree(GetProcessHeap(), 0, pbKey);
            }
        }
    }

    return hKeyContainer;
}

/******************************************************************************
 * build_hash_signature [Internal]
 *
 * Builds a padded version of a hash to match the length of the RSA key modulus.
 *
 * PARAMS
 *  pbSignature [O] The padded hash object is stored here.
 *  dwLen       [I] Length of the pbSignature buffer.
 *  aiAlgid     [I] Algorithm identifier of the hash to be padded.
 *  abHashValue [I] The value of the hash object.
 *  dwHashLen   [I] Length of the hash value.
 *  dwFlags     [I] Selection of padding algorithm.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE (NTE_BAD_ALGID)
 */
static BOOL build_hash_signature(BYTE *pbSignature, DWORD dwLen, ALG_ID aiAlgid, 
                                 CONST BYTE *abHashValue, DWORD dwHashLen, DWORD dwFlags) 
{
    /* These prefixes are meant to be concatenated with hash values of the
     * respective kind to form a PKCS #7 DigestInfo. */
    static const struct tagOIDDescriptor {
        ALG_ID aiAlgid;
        DWORD dwLen;
        CONST BYTE abOID[18];
    } aOIDDescriptor[5] = {
        { CALG_MD2, 18, { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48,
                          0x86, 0xf7, 0x0d, 0x02, 0x02, 0x05, 0x00, 0x04, 0x10 } },
        { CALG_MD4, 18, { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 
                          0x86, 0xf7, 0x0d, 0x02, 0x04, 0x05, 0x00, 0x04, 0x10 } },
        { CALG_MD5, 18, { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48,
                          0x86, 0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10 } },
        { CALG_SHA, 15, { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 
                          0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 } },
        { 0,        0,  {} }
    };
    DWORD dwIdxOID, i, j;

    for (dwIdxOID = 0; aOIDDescriptor[dwIdxOID].aiAlgid; dwIdxOID++) {
        if (aOIDDescriptor[dwIdxOID].aiAlgid == aiAlgid) break;
    }
    
    if (!aOIDDescriptor[dwIdxOID].aiAlgid) {
        SetLastError(NTE_BAD_ALGID);
        return FALSE;
    }

    /* Build the padded signature */
    if (dwFlags & CRYPT_X931_FORMAT) {
        pbSignature[0] = 0x6b;
        for (i=1; i < dwLen - dwHashLen - 3; i++) {
            pbSignature[i] = 0xbb;
        }
        pbSignature[i++] = 0xba;
        for (j=0; j < dwHashLen; j++, i++) {
            pbSignature[i] = abHashValue[j];
        }
        pbSignature[i++] = 0x33;
        pbSignature[i++] = 0xcc;
    } else {
        pbSignature[0] = 0x00;
        pbSignature[1] = 0x01;
        if (dwFlags & CRYPT_NOHASHOID) {
            for (i=2; i < dwLen - 1 - dwHashLen; i++) {
                pbSignature[i] = 0xff;
            }
            pbSignature[i++] = 0x00;
        } else {
            for (i=2; i < dwLen - 1 - aOIDDescriptor[dwIdxOID].dwLen - dwHashLen; i++) {
                pbSignature[i] = 0xff;
            }
            pbSignature[i++] = 0x00;
            for (j=0; j < aOIDDescriptor[dwIdxOID].dwLen; j++) {
                pbSignature[i++] = aOIDDescriptor[dwIdxOID].abOID[j];
            }
        }
        for (j=0; j < dwHashLen; j++) {
            pbSignature[i++] = abHashValue[j];
        }
    }
    
    return TRUE;
}

/******************************************************************************
 * tls1_p [Internal]
 *
 * This is an implementation of the 'P_hash' helper function for TLS1's PRF.
 * It is used exclusively by tls1_prf. For details see RFC 2246, chapter 5.
 * The pseudo random stream generated by this function is exclusive or'ed with
 * the data in pbBuffer.
 *
 * PARAMS
 *  hHMAC       [I]   HMAC object, which will be used in pseudo random generation
 *  pblobSeed   [I]   Seed value
 *  pbBuffer    [I/O] Pseudo random stream will be xor'ed to the provided data
 *  dwBufferLen [I]   Number of pseudo random bytes desired
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 */
static BOOL tls1_p(HCRYPTHASH hHMAC, CONST PCRYPT_DATA_BLOB pblobSeed, PBYTE pbBuffer, DWORD dwBufferLen)
{
    CRYPTHASH *pHMAC;
    BYTE abAi[RSAENH_MAX_HASH_SIZE];
    DWORD i = 0;

    if (!lookup_handle(&handle_table, hHMAC, RSAENH_MAGIC_HASH, (OBJECTHDR**)&pHMAC)) {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }
    
    /* compute A_1 = HMAC(seed) */
    init_hash(pHMAC);
    update_hash(pHMAC, pblobSeed->pbData, pblobSeed->cbData);
    finalize_hash(pHMAC);
    memcpy(abAi, pHMAC->abHashValue, pHMAC->dwHashSize);

    do {
        /* compute HMAC(A_i + seed) */
        init_hash(pHMAC);
        update_hash(pHMAC, abAi, pHMAC->dwHashSize);
        update_hash(pHMAC, pblobSeed->pbData, pblobSeed->cbData);
        finalize_hash(pHMAC);

        /* pseudo random stream := CONCAT_{i=1..n} ( HMAC(A_i + seed) ) */
        do {
            if (i >= dwBufferLen) break;
            pbBuffer[i] ^= pHMAC->abHashValue[i % pHMAC->dwHashSize];
            i++;
        } while (i % pHMAC->dwHashSize);

        /* compute A_{i+1} = HMAC(A_i) */
        init_hash(pHMAC);
        update_hash(pHMAC, abAi, pHMAC->dwHashSize);
        finalize_hash(pHMAC);
        memcpy(abAi, pHMAC->abHashValue, pHMAC->dwHashSize);
    } while (i < dwBufferLen);

    return TRUE;
}

/******************************************************************************
 * tls1_prf [Internal]
 *
 * TLS1 pseudo random function as specified in RFC 2246, chapter 5
 *
 * PARAMS
 *  hProv       [I] Key container used to compute the pseudo random stream
 *  hSecret     [I] Key that holds the (pre-)master secret
 *  pblobLabel  [I] Descriptive label
 *  pblobSeed   [I] Seed value
 *  pbBuffer    [O] Pseudo random numbers will be stored here
 *  dwBufferLen [I] Number of pseudo random bytes desired
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 */ 
static BOOL tls1_prf(HCRYPTPROV hProv, HCRYPTPROV hSecret, CONST PCRYPT_DATA_BLOB pblobLabel,
                     CONST PCRYPT_DATA_BLOB pblobSeed, PBYTE pbBuffer, DWORD dwBufferLen)
{
    HMAC_INFO hmacInfo = { 0, NULL, 0, NULL, 0 };
    HCRYPTHASH hHMAC = (HCRYPTHASH)INVALID_HANDLE_VALUE;
    HCRYPTKEY hHalfSecret = (HCRYPTKEY)INVALID_HANDLE_VALUE;
    CRYPTKEY *pHalfSecret, *pSecret;
    DWORD dwHalfSecretLen;
    BOOL result = FALSE;
    CRYPT_DATA_BLOB blobLabelSeed;

    TRACE("(hProv=%08lx, hSecret=%08lx, pblobLabel=%p, pblobSeed=%p, pbBuffer=%p, dwBufferLen=%ld)\n",
          hProv, hSecret, pblobLabel, pblobSeed, pbBuffer, dwBufferLen);

    if (!lookup_handle(&handle_table, hSecret, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pSecret)) {
        SetLastError(NTE_FAIL);
        return FALSE;
    }

    dwHalfSecretLen = (pSecret->dwKeyLen+1)/2;
    
    /* concatenation of the label and the seed */
    if (!concat_data_blobs(&blobLabelSeed, pblobLabel, pblobSeed)) goto exit;
   
    /* zero out the buffer, since two random streams will be xor'ed into it. */
    memset(pbBuffer, 0, dwBufferLen);
   
    /* build a 'fake' key, to hold the secret. CALG_SSL2_MASTER is used since it provides
     * the biggest range of valid key lengths. */
    hHalfSecret = new_key(hProv, CALG_SSL2_MASTER, MAKELONG(0,dwHalfSecretLen*8), &pHalfSecret);
    if (hHalfSecret == (HCRYPTKEY)INVALID_HANDLE_VALUE) goto exit;

    /* Derive an HMAC_MD5 hash and call the helper function. */
    memcpy(pHalfSecret->abKeyValue, pSecret->abKeyValue, dwHalfSecretLen);
    if (!RSAENH_CPCreateHash(hProv, CALG_HMAC, hHalfSecret, 0, &hHMAC)) goto exit;
    hmacInfo.HashAlgid = CALG_MD5;
    if (!RSAENH_CPSetHashParam(hProv, hHMAC, HP_HMAC_INFO, (BYTE*)&hmacInfo, 0)) goto exit;
    if (!tls1_p(hHMAC, &blobLabelSeed, pbBuffer, dwBufferLen)) goto exit;

    /* Reconfigure to HMAC_SHA hash and call helper function again. */
    memcpy(pHalfSecret->abKeyValue, pSecret->abKeyValue + (pSecret->dwKeyLen/2), dwHalfSecretLen);
    hmacInfo.HashAlgid = CALG_SHA;
    if (!RSAENH_CPSetHashParam(hProv, hHMAC, HP_HMAC_INFO, (BYTE*)&hmacInfo, 0)) goto exit;
    if (!tls1_p(hHMAC, &blobLabelSeed, pbBuffer, dwBufferLen)) goto exit;
    
    result = TRUE;
exit:
    release_handle(&handle_table, hHalfSecret, RSAENH_MAGIC_KEY);
    if (hHMAC != (HCRYPTHASH)INVALID_HANDLE_VALUE) RSAENH_CPDestroyHash(hProv, hHMAC);
    free_data_blob(&blobLabelSeed);
    return result;
}

/******************************************************************************
 * pad_data [Internal]
 *
 * Helper function for data padding according to PKCS1 #2
 *
 * PARAMS
 *  abData      [I] The data to be padded
 *  dwDataLen   [I] Length of the data 
 *  abBuffer    [O] Padded data will be stored here
 *  dwBufferLen [I] Length of the buffer (also length of padded data)
 *  dwFlags     [I] Padding format (CRYPT_SSL2_FALLBACK)
 *
 * RETURN
 *  Success: TRUE
 *  Failure: FALSE (NTE_BAD_LEN, too much data to pad)
 */
static BOOL pad_data(CONST BYTE *abData, DWORD dwDataLen, BYTE *abBuffer, DWORD dwBufferLen, 
                     DWORD dwFlags)
{
    DWORD i;
    
    /* Ensure there is enough space for PKCS1 #2 padding */
    if (dwDataLen > dwBufferLen-11) {
        SetLastError(NTE_BAD_LEN);
        return FALSE;
    }

    memmove(abBuffer + dwBufferLen - dwDataLen, abData, dwDataLen);            
    
    abBuffer[0] = 0x00;
    abBuffer[1] = RSAENH_PKC_BLOCKTYPE; 
    for (i=2; i < dwBufferLen - dwDataLen - 1; i++) 
        do gen_rand_impl(&abBuffer[i], 1); while (!abBuffer[i]);
    if (dwFlags & CRYPT_SSL2_FALLBACK) 
        for (i-=8; i < dwBufferLen - dwDataLen - 1; i++) 
            abBuffer[i] = 0x03;
    abBuffer[i] = 0x00;
    
    return TRUE; 
}

/******************************************************************************
 * unpad_data [Internal]
 *
 * Remove the PKCS1 padding from RSA decrypted data
 *
 * PARAMS
 *  abData      [I]   The padded data
 *  dwDataLen   [I]   Length of the padded data
 *  abBuffer    [O]   Data without padding will be stored here
 *  dwBufferLen [I/O] I: Length of the buffer, O: Length of unpadded data
 *  dwFlags     [I]   Currently none defined
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE, (NTE_BAD_DATA, no valid PKCS1 padding or buffer too small)
 */
static BOOL unpad_data(CONST BYTE *abData, DWORD dwDataLen, BYTE *abBuffer, DWORD *dwBufferLen, 
                       DWORD dwFlags)
{
    DWORD i;
    
    for (i=2; i<dwDataLen; i++)
        if (!abData[i])
            break;

    if ((i == dwDataLen) || (*dwBufferLen < dwDataLen - i - 1) ||
        (abData[0] != 0x00) || (abData[1] != RSAENH_PKC_BLOCKTYPE))
    {
        SetLastError(NTE_BAD_DATA);
        return FALSE;
    }

    *dwBufferLen = dwDataLen - i - 1;
    memmove(abBuffer, abData + i + 1, *dwBufferLen);
    return TRUE;
}

/******************************************************************************
 * CPAcquireContext (RSAENH.@)
 *
 * Acquire a handle to the key container specified by pszContainer
 *
 * PARAMS
 *  phProv       [O] Pointer to the location the acquired handle will be written to.
 *  pszContainer [I] Name of the desired key container. See Notes
 *  dwFlags      [I] Flags. See Notes.
 *  pVTable      [I] Pointer to a PVTableProvStruct containing callbacks.
 * 
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 *
 * NOTES
 *  If pszContainer is NULL or points to a zero length string the user's login 
 *  name will be used as the key container name.
 *
 *  If the CRYPT_NEW_KEYSET flag is set in dwFlags a new keyset will be created.
 *  If a keyset with the given name already exists, the function fails and sets
 *  last error to NTE_EXISTS. If CRYPT_NEW_KEYSET is not set and the specified
 *  key container does not exist, function fails and sets last error to 
 *  NTE_BAD_KEYSET.
 */                         
BOOL WINAPI RSAENH_CPAcquireContext(HCRYPTPROV *phProv, LPSTR pszContainer,
                   DWORD dwFlags, PVTableProvStruc pVTable)
{
    CHAR szKeyContainerName[MAX_PATH];
    CHAR szRegKey[MAX_PATH];

    TRACE("(phProv=%p, pszContainer=%s, dwFlags=%08lx, pVTable=%p)\n", phProv, 
          debugstr_a(pszContainer), dwFlags, pVTable);

    if (pszContainer && *pszContainer)
    {
        lstrcpynA(szKeyContainerName, pszContainer, MAX_PATH);
    } 
    else
    {
        DWORD dwLen = sizeof(szKeyContainerName);
        if (!GetUserNameA(szKeyContainerName, &dwLen)) return FALSE;
    }

    switch (dwFlags & (CRYPT_NEWKEYSET|CRYPT_VERIFYCONTEXT|CRYPT_DELETEKEYSET)) 
    {
        case 0:
            *phProv = read_key_container(szKeyContainerName, dwFlags, pVTable);
            break;

        case CRYPT_DELETEKEYSET:
            if (snprintf(szRegKey, MAX_PATH, RSAENH_REGKEY, pszContainer) >= MAX_PATH) {
                SetLastError(NTE_BAD_KEYSET_PARAM);
                return FALSE;
            } else {
                RegDeleteKeyA(HKEY_CURRENT_USER, szRegKey);
                SetLastError(ERROR_SUCCESS);
                return TRUE;
            }
            break;

        case CRYPT_NEWKEYSET:
            *phProv = read_key_container(szKeyContainerName, dwFlags, pVTable);
            if (*phProv != (HCRYPTPROV)INVALID_HANDLE_VALUE) 
            {
                release_handle(&handle_table, (unsigned int)*phProv, RSAENH_MAGIC_CONTAINER);
                SetLastError(NTE_EXISTS);
                return FALSE;
            }
            *phProv = new_key_container(szKeyContainerName, dwFlags, pVTable);
            break;

        case CRYPT_VERIFYCONTEXT:
            if (pszContainer) {
                SetLastError(NTE_BAD_FLAGS);
                return FALSE;
            }
            *phProv = new_key_container("", dwFlags, pVTable);
            break;
            
        default:
            *phProv = (unsigned int)INVALID_HANDLE_VALUE;
            SetLastError(NTE_BAD_FLAGS);
            return FALSE;
    }
                
    if (*phProv != (unsigned int)INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    } else {
        return FALSE;
    }
}

/******************************************************************************
 * CPCreateHash (RSAENH.@)
 *
 * CPCreateHash creates and initalizes a new hash object.
 *
 * PARAMS
 *  hProv   [I] Handle to the key container to which the new hash will belong.
 *  Algid   [I] Identifies the hash algorithm, which will be used for the hash.
 *  hKey    [I] Handle to a session key applied for keyed hashes.
 *  dwFlags [I] Currently no flags defined. Must be zero.
 *  phHash  [O] Points to the location where a handle to the new hash will be stored.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 *
 * NOTES
 *  hKey is a handle to a session key applied in keyed hashes like MAC and HMAC.
 *  If a normal hash object is to be created (like e.g. MD2 or SHA1) hKey must be zero.
 */
BOOL WINAPI RSAENH_CPCreateHash(HCRYPTPROV hProv, ALG_ID Algid, HCRYPTKEY hKey, DWORD dwFlags, 
                                HCRYPTHASH *phHash)
{
    CRYPTKEY *pCryptKey;
    CRYPTHASH *pCryptHash;
    const PROV_ENUMALGS_EX *peaAlgidInfo;
        
    TRACE("(hProv=%08lx, Algid=%08x, hKey=%08lx, dwFlags=%08lx, phHash=%p)\n", hProv, Algid, hKey, 
          dwFlags, phHash);

    peaAlgidInfo = get_algid_info(hProv, Algid);
    if (!peaAlgidInfo) return FALSE;

    if (dwFlags)
    {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    if (Algid == CALG_MAC || Algid == CALG_HMAC || Algid == CALG_SCHANNEL_MASTER_HASH || 
        Algid == CALG_TLS1PRF) 
    {
        if (!lookup_handle(&handle_table, hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pCryptKey)) {
            SetLastError(NTE_BAD_KEY);
            return FALSE;
        }

        if ((Algid == CALG_MAC) && (GET_ALG_TYPE(pCryptKey->aiAlgid) != ALG_TYPE_BLOCK)) {
            SetLastError(NTE_BAD_KEY);
            return FALSE;
        }

        if ((Algid == CALG_SCHANNEL_MASTER_HASH || Algid == CALG_TLS1PRF) && 
            (pCryptKey->aiAlgid != CALG_TLS1_MASTER)) 
        {
            SetLastError(NTE_BAD_KEY);
            return FALSE;
        }

        if ((Algid == CALG_TLS1PRF) && (pCryptKey->dwState != RSAENH_KEYSTATE_MASTERKEY)) {
            SetLastError(NTE_BAD_KEY_STATE);
            return FALSE;
        }
    }

    *phHash = (HCRYPTHASH)new_object(&handle_table, sizeof(CRYPTHASH), RSAENH_MAGIC_HASH,
                                     destroy_hash, (OBJECTHDR**)&pCryptHash);
    if (!pCryptHash) return FALSE;
    
    pCryptHash->aiAlgid = Algid;
    pCryptHash->hKey = hKey;
    pCryptHash->hProv = hProv;
    pCryptHash->dwState = RSAENH_HASHSTATE_IDLE;
    pCryptHash->pHMACInfo = (PHMAC_INFO)NULL;
    pCryptHash->dwHashSize = peaAlgidInfo->dwDefaultLen >> 3;
    init_data_blob(&pCryptHash->tpPRFParams.blobLabel);
    init_data_blob(&pCryptHash->tpPRFParams.blobSeed);

    if (Algid == CALG_SCHANNEL_MASTER_HASH) {
        static const char keyex[] = "key expansion";
        BYTE key_expansion[sizeof keyex];
        CRYPT_DATA_BLOB blobRandom, blobKeyExpansion = { 13, key_expansion };

        memcpy( key_expansion, keyex, sizeof keyex );
        
        if (pCryptKey->dwState != RSAENH_KEYSTATE_MASTERKEY) {
            static const char msec[] = "master secret";
            BYTE master_secret[sizeof msec];
            CRYPT_DATA_BLOB blobLabel = { 13, master_secret };
            BYTE abKeyValue[48];

            memcpy( master_secret, msec, sizeof msec );
    
            /* See RFC 2246, chapter 8.1 */
            if (!concat_data_blobs(&blobRandom, 
                                   &pCryptKey->siSChannelInfo.blobClientRandom, 
                                   &pCryptKey->siSChannelInfo.blobServerRandom))
            {
                return FALSE;
            }
            tls1_prf(hProv, hKey, &blobLabel, &blobRandom, abKeyValue, 48);
            pCryptKey->dwState = RSAENH_KEYSTATE_MASTERKEY; 
            memcpy(pCryptKey->abKeyValue, abKeyValue, 48);
            free_data_blob(&blobRandom);
        }

        /* See RFC 2246, chapter 6.3 */
        if (!concat_data_blobs(&blobRandom, 
                                  &pCryptKey->siSChannelInfo.blobServerRandom, 
                                  &pCryptKey->siSChannelInfo.blobClientRandom))
        {
            return FALSE;
        }
        tls1_prf(hProv, hKey, &blobKeyExpansion, &blobRandom, pCryptHash->abHashValue, 
                 RSAENH_MAX_HASH_SIZE);
        free_data_blob(&blobRandom);
    }

    return init_hash(pCryptHash);
}

/******************************************************************************
 * CPDestroyHash (RSAENH.@)
 * 
 * Releases the handle to a hash object. The object is destroyed if it's reference
 * count reaches zero.
 *
 * PARAMS
 *  hProv [I] Handle to the key container to which the hash object belongs.
 *  hHash [I] Handle to the hash object to be released.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE 
 */
BOOL WINAPI RSAENH_CPDestroyHash(HCRYPTPROV hProv, HCRYPTHASH hHash)
{
    TRACE("(hProv=%08lx, hHash=%08lx)\n", hProv, hHash);
     
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }
        
    if (!release_handle(&handle_table, hHash, RSAENH_MAGIC_HASH)) 
    {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }
    
    return TRUE;
}

/******************************************************************************
 * CPDestroyKey (RSAENH.@)
 *
 * Releases the handle to a key object. The object is destroyed if it's reference
 * count reaches zero.
 *
 * PARAMS
 *  hProv [I] Handle to the key container to which the key object belongs.
 *  hKey  [I] Handle to the key object to be released.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 */
BOOL WINAPI RSAENH_CPDestroyKey(HCRYPTPROV hProv, HCRYPTKEY hKey)
{
    TRACE("(hProv=%08lx, hKey=%08lx)\n", hProv, hKey);
        
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }
        
    if (!release_handle(&handle_table, hKey, RSAENH_MAGIC_KEY)) 
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }
    
    return TRUE;
}

/******************************************************************************
 * CPDuplicateHash (RSAENH.@)
 *
 * Clones a hash object including it's current state.
 *
 * PARAMS
 *  hUID        [I] Handle to the key container the hash belongs to.
 *  hHash       [I] Handle to the hash object to be cloned.
 *  pdwReserved [I] Reserved. Must be NULL.
 *  dwFlags     [I] No flags are currently defined. Must be 0.
 *  phHash      [O] Handle to the cloned hash object.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 */
BOOL WINAPI RSAENH_CPDuplicateHash(HCRYPTPROV hUID, HCRYPTHASH hHash, DWORD *pdwReserved, 
                                   DWORD dwFlags, HCRYPTHASH *phHash)
{
    CRYPTHASH *pSrcHash, *pDestHash;
    
    TRACE("(hUID=%08lx, hHash=%08lx, pdwReserved=%p, dwFlags=%08lx, phHash=%p)\n", hUID, hHash, 
           pdwReserved, dwFlags, phHash);

    if (!is_valid_handle(&handle_table, hUID, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, hHash, RSAENH_MAGIC_HASH, (OBJECTHDR**)&pSrcHash))
    {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }

    if (!phHash || pdwReserved || dwFlags) 
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *phHash = (HCRYPTHASH)new_object(&handle_table, sizeof(CRYPTHASH), RSAENH_MAGIC_HASH, 
                                     destroy_hash, (OBJECTHDR**)&pDestHash);
    if (*phHash != (HCRYPTHASH)INVALID_HANDLE_VALUE)
    {
        memcpy(pDestHash, pSrcHash, sizeof(CRYPTHASH));
        duplicate_hash_impl(pSrcHash->aiAlgid, &pSrcHash->context, &pDestHash->context);
        copy_hmac_info(&pDestHash->pHMACInfo, pSrcHash->pHMACInfo);
        copy_data_blob(&pDestHash->tpPRFParams.blobLabel, &pSrcHash->tpPRFParams.blobLabel);
        copy_data_blob(&pDestHash->tpPRFParams.blobSeed, &pSrcHash->tpPRFParams.blobSeed);
    }

    return *phHash != (HCRYPTHASH)INVALID_HANDLE_VALUE;
}

/******************************************************************************
 * CPDuplicateKey (RSAENH.@)
 *
 * Clones a key object including it's current state.
 *
 * PARAMS
 *  hUID        [I] Handle to the key container the hash belongs to.
 *  hKey        [I] Handle to the key object to be cloned.
 *  pdwReserved [I] Reserved. Must be NULL.
 *  dwFlags     [I] No flags are currently defined. Must be 0.
 *  phHash      [O] Handle to the cloned key object.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 */
BOOL WINAPI RSAENH_CPDuplicateKey(HCRYPTPROV hUID, HCRYPTKEY hKey, DWORD *pdwReserved, 
                                  DWORD dwFlags, HCRYPTKEY *phKey)
{
    CRYPTKEY *pSrcKey, *pDestKey;
    
    TRACE("(hUID=%08lx, hKey=%08lx, pdwReserved=%p, dwFlags=%08lx, phKey=%p)\n", hUID, hKey, 
          pdwReserved, dwFlags, phKey);

    if (!is_valid_handle(&handle_table, hUID, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pSrcKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    if (!phKey || pdwReserved || dwFlags) 
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    *phKey = (HCRYPTKEY)new_object(&handle_table, sizeof(CRYPTKEY), RSAENH_MAGIC_KEY, destroy_key, 
                                   (OBJECTHDR**)&pDestKey);
    if (*phKey != (HCRYPTKEY)INVALID_HANDLE_VALUE)
    {
        memcpy(pDestKey, pSrcKey, sizeof(CRYPTKEY));
        copy_data_blob(&pDestKey->siSChannelInfo.blobServerRandom,
                       &pSrcKey->siSChannelInfo.blobServerRandom);
        copy_data_blob(&pDestKey->siSChannelInfo.blobClientRandom, 
                       &pSrcKey->siSChannelInfo.blobClientRandom);
        duplicate_key_impl(pSrcKey->aiAlgid, &pSrcKey->context, &pDestKey->context);
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

/******************************************************************************
 * CPEncrypt (RSAENH.@)
 *
 * Encrypt data.
 *
 * PARAMS
 *  hProv      [I]   The key container hKey and hHash belong to.
 *  hKey       [I]   The key used to encrypt the data.
 *  hHash      [I]   An optional hash object for parallel hashing. See notes.
 *  Final      [I]   Indicates if this is the last block of data to encrypt.
 *  dwFlags    [I]   Currently no flags defined. Must be zero.
 *  pbData     [I/O] Pointer to the data to encrypt. Encrypted data will also be stored there. 
 *  pdwDataLen [I/O] I: Length of data to encrypt, O: Length of encrypted data.
 *  dwBufLen   [I]   Size of the buffer at pbData.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * NOTES
 *  If a hash object handle is provided in hHash, it will be updated with the plaintext. 
 *  This is useful for message signatures.
 *
 *  This function uses the standard WINAPI protocol for querying data of dynamic length. 
 */
BOOL WINAPI RSAENH_CPEncrypt(HCRYPTPROV hProv, HCRYPTKEY hKey, HCRYPTHASH hHash, BOOL Final, 
                             DWORD dwFlags, BYTE *pbData, DWORD *pdwDataLen, DWORD dwBufLen)
{
    CRYPTKEY *pCryptKey;
    BYTE *in, out[RSAENH_MAX_BLOCK_SIZE], o[RSAENH_MAX_BLOCK_SIZE];
    DWORD dwEncryptedLen, i, j, k;
        
    TRACE("(hProv=%08lx, hKey=%08lx, hHash=%08lx, Final=%d, dwFlags=%08lx, pbData=%p, "
          "pdwDataLen=%p, dwBufLen=%ld)\n", hProv, hKey, hHash, Final, dwFlags, pbData, pdwDataLen,
          dwBufLen);
    
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags)
    {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    if (pCryptKey->dwState == RSAENH_KEYSTATE_IDLE) 
        pCryptKey->dwState = RSAENH_KEYSTATE_ENCRYPTING;

    if (pCryptKey->dwState != RSAENH_KEYSTATE_ENCRYPTING) 
    {
        SetLastError(NTE_BAD_DATA);
        return FALSE;
    }

    if (is_valid_handle(&handle_table, hHash, RSAENH_MAGIC_HASH)) {
        if (!RSAENH_CPHashData(hProv, hHash, pbData, *pdwDataLen, 0)) return FALSE;
    }
    
    if (GET_ALG_TYPE(pCryptKey->aiAlgid) == ALG_TYPE_BLOCK) {
        if (!Final && (*pdwDataLen % pCryptKey->dwBlockLen)) {
            SetLastError(NTE_BAD_DATA);
            return FALSE;
        }

        dwEncryptedLen = (*pdwDataLen/pCryptKey->dwBlockLen+(Final?1:0))*pCryptKey->dwBlockLen;
        for (i=*pdwDataLen; i<dwEncryptedLen && i<dwBufLen; i++) pbData[i] = dwEncryptedLen - *pdwDataLen;
        *pdwDataLen = dwEncryptedLen; 

        if (*pdwDataLen > dwBufLen) 
        {
            SetLastError(ERROR_MORE_DATA);
            return FALSE;
        }
    
        for (i=0, in=pbData; i<*pdwDataLen; i+=pCryptKey->dwBlockLen, in+=pCryptKey->dwBlockLen) {
            switch (pCryptKey->dwMode) {
                case CRYPT_MODE_ECB:
                    encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, in, out, 
                                       RSAENH_ENCRYPT);
                    break;
                
                case CRYPT_MODE_CBC:
                    for (j=0; j<pCryptKey->dwBlockLen; j++) in[j] ^= pCryptKey->abChainVector[j];
                    encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, in, out, 
                                       RSAENH_ENCRYPT);
                    memcpy(pCryptKey->abChainVector, out, pCryptKey->dwBlockLen);
                    break;

                case CRYPT_MODE_CFB:
                    for (j=0; j<pCryptKey->dwBlockLen; j++) {
                        encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, 
                                           pCryptKey->abChainVector, o, RSAENH_ENCRYPT);
                        out[j] = in[j] ^ o[0];
                        for (k=0; k<pCryptKey->dwBlockLen-1; k++) 
                            pCryptKey->abChainVector[k] = pCryptKey->abChainVector[k+1];
                        pCryptKey->abChainVector[k] = out[j];
                    }
                    break;
                    
                default:
                    SetLastError(NTE_BAD_ALGID);
                    return FALSE;
            }
            memcpy(in, out, pCryptKey->dwBlockLen); 
        }
    } else if (GET_ALG_TYPE(pCryptKey->aiAlgid) == ALG_TYPE_STREAM) {
        encrypt_stream_impl(pCryptKey->aiAlgid, &pCryptKey->context, pbData, *pdwDataLen);
    } else if (GET_ALG_TYPE(pCryptKey->aiAlgid) == ALG_TYPE_RSA) {
        if (pCryptKey->aiAlgid == CALG_RSA_SIGN) {
            SetLastError(NTE_BAD_KEY);
            return FALSE;
        }
        if (dwBufLen < pCryptKey->dwBlockLen) {
            SetLastError(ERROR_MORE_DATA);
            return FALSE;
        }
        if (!pad_data(pbData, *pdwDataLen, pbData, pCryptKey->dwBlockLen, dwFlags)) return FALSE;
        encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, pbData, pbData, RSAENH_ENCRYPT);
        *pdwDataLen = pCryptKey->dwBlockLen;
        Final = TRUE;
    } else {
        SetLastError(NTE_BAD_TYPE);
        return FALSE;
    }

    if (Final) setup_key(pCryptKey);

    return TRUE;
}

/******************************************************************************
 * CPDecrypt (RSAENH.@)
 *
 * Decrypt data.
 *
 * PARAMS
 *  hProv      [I]   The key container hKey and hHash belong to.
 *  hKey       [I]   The key used to decrypt the data.
 *  hHash      [I]   An optional hash object for parallel hashing. See notes.
 *  Final      [I]   Indicates if this is the last block of data to decrypt.
 *  dwFlags    [I]   Currently no flags defined. Must be zero.
 *  pbData     [I/O] Pointer to the data to decrypt. Plaintext will also be stored there. 
 *  pdwDataLen [I/O] I: Length of ciphertext, O: Length of plaintext.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * NOTES
 *  If a hash object handle is provided in hHash, it will be updated with the plaintext. 
 *  This is useful for message signatures.
 *
 *  This function uses the standard WINAPI protocol for querying data of dynamic length. 
 */
BOOL WINAPI RSAENH_CPDecrypt(HCRYPTPROV hProv, HCRYPTKEY hKey, HCRYPTHASH hHash, BOOL Final, 
                             DWORD dwFlags, BYTE *pbData, DWORD *pdwDataLen)
{
    CRYPTKEY *pCryptKey;
    BYTE *in, out[RSAENH_MAX_BLOCK_SIZE], o[RSAENH_MAX_BLOCK_SIZE];
    DWORD i, j, k;
    DWORD dwMax;

    TRACE("(hProv=%08lx, hKey=%08lx, hHash=%08lx, Final=%d, dwFlags=%08lx, pbData=%p, "
          "pdwDataLen=%p)\n", hProv, hKey, hHash, Final, dwFlags, pbData, pdwDataLen);
    
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags)
    {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    if (pCryptKey->dwState == RSAENH_KEYSTATE_IDLE) 
        pCryptKey->dwState = RSAENH_KEYSTATE_DECRYPTING;

    if (pCryptKey->dwState != RSAENH_KEYSTATE_DECRYPTING)
    {
        SetLastError(NTE_BAD_DATA);
        return FALSE;
    }

    dwMax=*pdwDataLen;

    if (GET_ALG_TYPE(pCryptKey->aiAlgid) == ALG_TYPE_BLOCK) {
        for (i=0, in=pbData; i<*pdwDataLen; i+=pCryptKey->dwBlockLen, in+=pCryptKey->dwBlockLen) {
            switch (pCryptKey->dwMode) {
                case CRYPT_MODE_ECB:
                    encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, in, out, 
                                       RSAENH_DECRYPT);
                    break;
                
                case CRYPT_MODE_CBC:
                    encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, in, out, 
                                       RSAENH_DECRYPT);
                    for (j=0; j<pCryptKey->dwBlockLen; j++) out[j] ^= pCryptKey->abChainVector[j];
                    memcpy(pCryptKey->abChainVector, in, pCryptKey->dwBlockLen);
                    break;

                case CRYPT_MODE_CFB:
                    for (j=0; j<pCryptKey->dwBlockLen; j++) {
                        encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, 
                                           pCryptKey->abChainVector, o, RSAENH_ENCRYPT);
                        out[j] = in[j] ^ o[0];
                        for (k=0; k<pCryptKey->dwBlockLen-1; k++) 
                            pCryptKey->abChainVector[k] = pCryptKey->abChainVector[k+1];
                        pCryptKey->abChainVector[k] = in[j];
                    }
                    break;
                    
                default:
                    SetLastError(NTE_BAD_ALGID);
                    return FALSE;
            }
            memcpy(in, out, pCryptKey->dwBlockLen);
        }
        if (Final) *pdwDataLen -= pbData[*pdwDataLen-1]; 

    } else if (GET_ALG_TYPE(pCryptKey->aiAlgid) == ALG_TYPE_STREAM) {
        encrypt_stream_impl(pCryptKey->aiAlgid, &pCryptKey->context, pbData, *pdwDataLen);
    } else if (GET_ALG_TYPE(pCryptKey->aiAlgid) == ALG_TYPE_RSA) {
        if (pCryptKey->aiAlgid == CALG_RSA_SIGN) {
            SetLastError(NTE_BAD_KEY);
            return FALSE;
        }
        encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, pbData, pbData, RSAENH_DECRYPT);
        if (!unpad_data(pbData, pCryptKey->dwBlockLen, pbData, pdwDataLen, dwFlags)) return FALSE;
        Final = TRUE;
    } else {
        SetLastError(NTE_BAD_TYPE);
        return FALSE;
    } 
    
    if (Final) setup_key(pCryptKey);

    if (is_valid_handle(&handle_table, hHash, RSAENH_MAGIC_HASH)) {
        if (*pdwDataLen>dwMax ||
            !RSAENH_CPHashData(hProv, hHash, pbData, *pdwDataLen, 0)) return FALSE;
    }
    
    return TRUE;
}

/******************************************************************************
 * CPExportKey (RSAENH.@)
 *
 * Export a key into a binary large object (BLOB).
 *
 * PARAMS
 *  hProv      [I]   Key container from which a key is to be exported.
 *  hKey       [I]   Key to be exported.
 *  hPubKey    [I]   Key used to encrypt sensitive BLOB data.
 *  dwBlobType [I]   SIMPLEBLOB, PUBLICKEYBLOB or PRIVATEKEYBLOB.
 *  dwFlags    [I]   Currently none defined.
 *  pbData     [O]   Pointer to a buffer where the BLOB will be written to.
 *  pdwDataLen [I/O] I: Size of buffer at pbData, O: Size of BLOB
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 */
BOOL WINAPI RSAENH_CPExportKey(HCRYPTPROV hProv, HCRYPTKEY hKey, HCRYPTKEY hPubKey, 
                               DWORD dwBlobType, DWORD dwFlags, BYTE *pbData, DWORD *pdwDataLen)
{
    CRYPTKEY *pCryptKey, *pPubKey;
    BLOBHEADER *pBlobHeader = (BLOBHEADER*)pbData;
    RSAPUBKEY *pRSAPubKey = (RSAPUBKEY*)(pBlobHeader+1);
    ALG_ID *pAlgid = (ALG_ID*)(pBlobHeader+1);
    DWORD dwDataLen;
    
    TRACE("(hProv=%08lx, hKey=%08lx, hPubKey=%08lx, dwBlobType=%08lx, dwFlags=%08lx, pbData=%p,"
          "pdwDataLen=%p)\n", hProv, hKey, hPubKey, dwBlobType, dwFlags, pbData, pdwDataLen);
    
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    if (dwFlags & CRYPT_SSL2_FALLBACK) {
        if (pCryptKey->aiAlgid != CALG_SSL2_MASTER) {
            SetLastError(NTE_BAD_KEY);
            return FALSE;
        }
    }
    
    switch ((BYTE)dwBlobType)
    {
        case SIMPLEBLOB:
            if (!lookup_handle(&handle_table, hPubKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pPubKey)){
                SetLastError(NTE_BAD_PUBLIC_KEY); /* FIXME: error_code? */
                return FALSE;
            }

            if (!(GET_ALG_CLASS(pCryptKey->aiAlgid)&(ALG_CLASS_DATA_ENCRYPT|ALG_CLASS_MSG_ENCRYPT))) {
                SetLastError(NTE_BAD_KEY); /* FIXME: error code? */
                return FALSE;
            }

            dwDataLen = sizeof(BLOBHEADER) + sizeof(ALG_ID) + pPubKey->dwBlockLen;
            if (pbData) {
                if (*pdwDataLen < dwDataLen) {
                    SetLastError(ERROR_MORE_DATA);
                    *pdwDataLen = dwDataLen;
                    return FALSE;
                }

                pBlobHeader->bType = SIMPLEBLOB;
                pBlobHeader->bVersion = CUR_BLOB_VERSION;
                pBlobHeader->reserved = 0;
                pBlobHeader->aiKeyAlg = pCryptKey->aiAlgid;

                *pAlgid = pPubKey->aiAlgid;
       
                if (!pad_data(pCryptKey->abKeyValue, pCryptKey->dwKeyLen, (BYTE*)(pAlgid+1), 
                              pPubKey->dwBlockLen, dwFlags))
                {
                    return FALSE;
                }
                
                encrypt_block_impl(pPubKey->aiAlgid, &pPubKey->context, (BYTE*)(pAlgid+1), 
                                   (BYTE*)(pAlgid+1), RSAENH_ENCRYPT); 
            }
            *pdwDataLen = dwDataLen;
            return TRUE;
            
        case PUBLICKEYBLOB:
            if (is_valid_handle(&handle_table, hPubKey, RSAENH_MAGIC_KEY)) {
                SetLastError(NTE_BAD_KEY); /* FIXME: error code? */
                return FALSE;
            }

            if ((pCryptKey->aiAlgid != CALG_RSA_KEYX) && (pCryptKey->aiAlgid != CALG_RSA_SIGN)) {
                SetLastError(NTE_BAD_KEY);
                return FALSE;
            }

            dwDataLen = sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + pCryptKey->dwKeyLen;
            if (pbData) {
                if (*pdwDataLen < dwDataLen) {
                    SetLastError(ERROR_MORE_DATA);
                    *pdwDataLen = dwDataLen;
                    return FALSE;
                }

                pBlobHeader->bType = PUBLICKEYBLOB;
                pBlobHeader->bVersion = CUR_BLOB_VERSION;
                pBlobHeader->reserved = 0;
                pBlobHeader->aiKeyAlg = pCryptKey->aiAlgid;

                pRSAPubKey->magic = RSAENH_MAGIC_RSA1; 
                pRSAPubKey->bitlen = pCryptKey->dwKeyLen << 3;
        
                export_public_key_impl((BYTE*)(pRSAPubKey+1), &pCryptKey->context, 
                                       pCryptKey->dwKeyLen, &pRSAPubKey->pubexp);
            }
            *pdwDataLen = dwDataLen;
            return TRUE;

        case PRIVATEKEYBLOB:
            if ((pCryptKey->aiAlgid != CALG_RSA_KEYX) && (pCryptKey->aiAlgid != CALG_RSA_SIGN)) {
                SetLastError(NTE_BAD_KEY);
                return FALSE;
            }
    
            dwDataLen = sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + 
                        2 * pCryptKey->dwKeyLen + 5 * ((pCryptKey->dwKeyLen + 1) >> 1);
            if (pbData) {
                if (*pdwDataLen < dwDataLen) {
                    SetLastError(ERROR_MORE_DATA);
                    *pdwDataLen = dwDataLen;
                    return FALSE;
                }
                
                pBlobHeader->bType = PRIVATEKEYBLOB;
                pBlobHeader->bVersion = CUR_BLOB_VERSION;
                pBlobHeader->reserved = 0;
                pBlobHeader->aiKeyAlg = pCryptKey->aiAlgid;

                pRSAPubKey->magic = RSAENH_MAGIC_RSA2;
                pRSAPubKey->bitlen = pCryptKey->dwKeyLen << 3;
                
                export_private_key_impl((BYTE*)(pRSAPubKey+1), &pCryptKey->context, 
                                        pCryptKey->dwKeyLen, &pRSAPubKey->pubexp);
            }
            *pdwDataLen = dwDataLen;
            return TRUE;
            
        default:
            SetLastError(NTE_BAD_TYPE); /* FIXME: error code? */
            return FALSE;
    }
}

/******************************************************************************
 * CPImportKey (RSAENH.@)
 *
 * Import a BLOB'ed key into a key container.
 *
 * PARAMS
 *  hProv     [I] Key container into which the key is to be imported.
 *  pbData    [I] Pointer to a buffer which holds the BLOB.
 *  dwDataLen [I] Length of data in buffer at pbData.
 *  hPubKey   [I] Key used to decrypt sensitive BLOB data.
 *  dwFlags   [I] Currently none defined.
 *  phKey     [O] Handle to the imported key.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 */
BOOL WINAPI RSAENH_CPImportKey(HCRYPTPROV hProv, CONST BYTE *pbData, DWORD dwDataLen, 
                               HCRYPTKEY hPubKey, DWORD dwFlags, HCRYPTKEY *phKey)
{
    CRYPTKEY *pCryptKey, *pPubKey;
    CONST BLOBHEADER *pBlobHeader = (CONST BLOBHEADER*)pbData;
    CONST RSAPUBKEY *pRSAPubKey = (CONST RSAPUBKEY*)(pBlobHeader+1);
    CONST ALG_ID *pAlgid = (CONST ALG_ID*)(pBlobHeader+1);
    CONST BYTE *pbKeyStream = (CONST BYTE*)(pAlgid + 1);
    ALG_ID algID;
    BYTE *pbDecrypted;
    DWORD dwKeyLen;

    TRACE("(hProv=%08lx, pbData=%p, dwDataLen=%ld, hPubKey=%08lx, dwFlags=%08lx, phKey=%p)\n", 
        hProv, pbData, dwDataLen, hPubKey, dwFlags, phKey);
    
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwDataLen < sizeof(BLOBHEADER) || 
        pBlobHeader->bVersion != CUR_BLOB_VERSION ||
        pBlobHeader->reserved != 0) 
    {
        SetLastError(NTE_BAD_DATA);
        return FALSE;
    }

    switch (pBlobHeader->bType)
    {
        case PRIVATEKEYBLOB:    
            if ((dwDataLen < sizeof(BLOBHEADER) + sizeof(RSAPUBKEY)) || 
                (pRSAPubKey->magic != RSAENH_MAGIC_RSA2) ||
                (dwDataLen < sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + 
                    (2 * pRSAPubKey->bitlen >> 3) + (5 * ((pRSAPubKey->bitlen+8)>>4)))) 
            {
                SetLastError(NTE_BAD_DATA);
                return FALSE;
            }
    
            *phKey = new_key(hProv, pBlobHeader->aiKeyAlg, MAKELONG(0,pRSAPubKey->bitlen), &pCryptKey);
            if (*phKey == (HCRYPTKEY)INVALID_HANDLE_VALUE) return FALSE;
            setup_key(pCryptKey);
            return import_private_key_impl((CONST BYTE*)(pRSAPubKey+1), &pCryptKey->context, 
                                           pRSAPubKey->bitlen/8, pRSAPubKey->pubexp);
                
        case PUBLICKEYBLOB:
            if ((dwDataLen < sizeof(BLOBHEADER) + sizeof(RSAPUBKEY)) || 
                (pRSAPubKey->magic != RSAENH_MAGIC_RSA1) ||
                (dwDataLen < sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + (pRSAPubKey->bitlen >> 3))) 
            {
                SetLastError(NTE_BAD_DATA);
                return FALSE;
            }
    
            /* Since this is a public key blob, only the public key is
             * available, so only signature verification is possible.
             */
            algID = pBlobHeader->aiKeyAlg;
            if (algID == CALG_RSA_KEYX)
                algID = CALG_RSA_SIGN;
            *phKey = new_key(hProv, algID, MAKELONG(0,pRSAPubKey->bitlen), &pCryptKey); 
            if (*phKey == (HCRYPTKEY)INVALID_HANDLE_VALUE) return FALSE; 
            setup_key(pCryptKey);
            return import_public_key_impl((CONST BYTE*)(pRSAPubKey+1), &pCryptKey->context, 
                                          pRSAPubKey->bitlen >> 3, pRSAPubKey->pubexp);
                
        case SIMPLEBLOB:
            if (!lookup_handle(&handle_table, hPubKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pPubKey) ||
                pPubKey->aiAlgid != CALG_RSA_KEYX) 
            {
                SetLastError(NTE_BAD_PUBLIC_KEY); /* FIXME: error code? */
                return FALSE;
            }

            if (dwDataLen < sizeof(BLOBHEADER)+sizeof(ALG_ID)+pPubKey->dwBlockLen) 
            {
                SetLastError(NTE_BAD_DATA); /* FIXME: error code */
                return FALSE;
            }

            pbDecrypted = HeapAlloc(GetProcessHeap(), 0, pPubKey->dwBlockLen);
            if (!pbDecrypted) return FALSE;
            encrypt_block_impl(pPubKey->aiAlgid, &pPubKey->context, pbKeyStream, pbDecrypted, 
                               RSAENH_DECRYPT);

            dwKeyLen = RSAENH_MAX_KEY_SIZE;
            if (!unpad_data(pbDecrypted, pPubKey->dwBlockLen, pbDecrypted, &dwKeyLen, dwFlags)) {
                HeapFree(GetProcessHeap(), 0, pbDecrypted);
                return FALSE;
            }
            
            *phKey = new_key(hProv, pBlobHeader->aiKeyAlg, dwKeyLen<<19, &pCryptKey);
            if (*phKey == (HCRYPTKEY)INVALID_HANDLE_VALUE)
            {
                HeapFree(GetProcessHeap(), 0, pbDecrypted);
                return FALSE;
            }
            memcpy(pCryptKey->abKeyValue, pbDecrypted, dwKeyLen);
            HeapFree(GetProcessHeap(), 0, pbDecrypted);
            setup_key(pCryptKey);
            return TRUE;

        default:
            SetLastError(NTE_BAD_TYPE); /* FIXME: error code? */
            return FALSE;
    }
}

/******************************************************************************
 * CPGenKey (RSAENH.@)
 *
 * Generate a key in the key container
 *
 * PARAMS
 *  hProv   [I] Key container for which a key is to be generated.
 *  Algid   [I] Crypto algorithm identifier for the key to be generated.
 *  dwFlags [I] Upper 16 bits: Binary length of key. Lower 16 bits: Flags. See Notes
 *  phKey   [O] Handle to the generated key.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * FIXME
 *  Flags currently not considered.
 *
 * NOTES
 *  Private key-exchange- and signature-keys can be generated with Algid AT_KEYEXCHANGE
 *  and AT_SIGNATURE values.
 */
BOOL WINAPI RSAENH_CPGenKey(HCRYPTPROV hProv, ALG_ID Algid, DWORD dwFlags, HCRYPTKEY *phKey)
{
    KEYCONTAINER *pKeyContainer;
    CRYPTKEY *pCryptKey;

    TRACE("(hProv=%08lx, aiAlgid=%d, dwFlags=%08lx, phKey=%p)\n", hProv, Algid, dwFlags, phKey);

    if (!lookup_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER, 
                       (OBJECTHDR**)&pKeyContainer)) 
    {
        /* MSDN: hProv not containing valid context handle */
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }
    
    switch (Algid)
    {
        case AT_SIGNATURE:
        case CALG_RSA_SIGN:
            *phKey = new_key(hProv, CALG_RSA_SIGN, dwFlags, &pCryptKey);
            if (pCryptKey) { 
                new_key_impl(pCryptKey->aiAlgid, &pCryptKey->context, pCryptKey->dwKeyLen);
                setup_key(pCryptKey);
                if (Algid == AT_SIGNATURE) {
                    RSAENH_CPDestroyKey(hProv, pKeyContainer->hSignatureKeyPair);
                    copy_handle(&handle_table, *phKey, RSAENH_MAGIC_KEY,
                                (unsigned int*)&pKeyContainer->hSignatureKeyPair);
                }
            }
            break;

        case AT_KEYEXCHANGE:
        case CALG_RSA_KEYX:
            *phKey = new_key(hProv, CALG_RSA_KEYX, dwFlags, &pCryptKey);
            if (pCryptKey) { 
                new_key_impl(pCryptKey->aiAlgid, &pCryptKey->context, pCryptKey->dwKeyLen);
                setup_key(pCryptKey);
                if (Algid == AT_KEYEXCHANGE) {
                    RSAENH_CPDestroyKey(hProv, pKeyContainer->hKeyExchangeKeyPair);
                    copy_handle(&handle_table, *phKey, RSAENH_MAGIC_KEY,
                                (unsigned int*)&pKeyContainer->hKeyExchangeKeyPair);
                }
            }
            break;
            
        case CALG_RC2:
        case CALG_RC4:
        case CALG_DES:
        case CALG_3DES_112:
        case CALG_3DES:
        case CALG_PCT1_MASTER:
        case CALG_SSL2_MASTER:
        case CALG_SSL3_MASTER:
        case CALG_TLS1_MASTER:
            *phKey = new_key(hProv, Algid, dwFlags, &pCryptKey);
            if (pCryptKey) {
                gen_rand_impl(pCryptKey->abKeyValue, RSAENH_MAX_KEY_SIZE);
                switch (Algid) {
                    case CALG_SSL3_MASTER:
                        pCryptKey->abKeyValue[0] = RSAENH_SSL3_VERSION_MAJOR;
                        pCryptKey->abKeyValue[1] = RSAENH_SSL3_VERSION_MINOR;
                        break;

                    case CALG_TLS1_MASTER:
                        pCryptKey->abKeyValue[0] = RSAENH_TLS1_VERSION_MAJOR;
                        pCryptKey->abKeyValue[1] = RSAENH_TLS1_VERSION_MINOR;
                        break;
                }
                setup_key(pCryptKey);
            }
            break;
            
        default:
            /* MSDN: Algorithm not supported specified by Algid */
            SetLastError(NTE_BAD_ALGID);
            return FALSE;
    }
            
    return *phKey != (unsigned int)INVALID_HANDLE_VALUE;
}

/******************************************************************************
 * CPGenRandom (RSAENH.@)
 *
 * Generate a random byte stream.
 *
 * PARAMS
 *  hProv    [I] Key container that is used to generate random bytes.
 *  dwLen    [I] Specifies the number of requested random data bytes.
 *  pbBuffer [O] Random bytes will be stored here.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 */
BOOL WINAPI RSAENH_CPGenRandom(HCRYPTPROV hProv, DWORD dwLen, BYTE *pbBuffer)
{
    TRACE("(hProv=%08lx, dwLen=%ld, pbBuffer=%p)\n", hProv, dwLen, pbBuffer);
    
    if (!is_valid_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER)) 
    {
        /* MSDN: hProv not containing valid context handle */
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    return gen_rand_impl(pbBuffer, dwLen);
}

/******************************************************************************
 * CPGetHashParam (RSAENH.@)
 *
 * Query parameters of an hash object.
 *
 * PARAMS
 *  hProv      [I]   The kea container, which the hash belongs to.
 *  hHash      [I]   The hash object that is to be queried.
 *  dwParam    [I]   Specifies the parameter that is to be queried.
 *  pbData     [I]   Pointer to the buffer where the parameter value will be stored.
 *  pdwDataLen [I/O] I: Buffer length at pbData, O: Length of the parameter value.
 *  dwFlags    [I]   None currently defined.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 *
 * NOTES
 *  Valid dwParams are: HP_ALGID, HP_HASHSIZE, HP_HASHVALUE. The hash will be 
 *  finalized if HP_HASHVALUE is queried.
 */
BOOL WINAPI RSAENH_CPGetHashParam(HCRYPTPROV hProv, HCRYPTHASH hHash, DWORD dwParam, BYTE *pbData, 
                                  DWORD *pdwDataLen, DWORD dwFlags) 
{
    CRYPTHASH *pCryptHash;
        
    TRACE("(hProv=%08lx, hHash=%08lx, dwParam=%08lx, pbData=%p, pdwDataLen=%p, dwFlags=%08lx)\n", 
        hProv, hHash, dwParam, pbData, pdwDataLen, dwFlags);
    
    if (!is_valid_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER)) 
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags)
    {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    
    if (!lookup_handle(&handle_table, (unsigned int)hHash, RSAENH_MAGIC_HASH, 
                       (OBJECTHDR**)&pCryptHash))
    {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }

    if (!pdwDataLen)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    
    switch (dwParam)
    {
        case HP_ALGID:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&pCryptHash->aiAlgid, 
                              sizeof(ALG_ID));

        case HP_HASHSIZE:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&pCryptHash->dwHashSize, 
                              sizeof(DWORD));

        case HP_HASHVAL:
            if (pCryptHash->aiAlgid == CALG_TLS1PRF) {
                return tls1_prf(hProv, pCryptHash->hKey, &pCryptHash->tpPRFParams.blobLabel,
                                &pCryptHash->tpPRFParams.blobSeed, pbData, *pdwDataLen);
            }
            
            if (pCryptHash->dwState == RSAENH_HASHSTATE_IDLE) {
                SetLastError(NTE_BAD_HASH_STATE);
                return FALSE;
            }
            
            if (pbData && (pCryptHash->dwState != RSAENH_HASHSTATE_FINISHED))
            {
                finalize_hash(pCryptHash);
                pCryptHash->dwState = RSAENH_HASHSTATE_FINISHED;
            }
            
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)pCryptHash->abHashValue, 
                              pCryptHash->dwHashSize);

        default:
            SetLastError(NTE_BAD_TYPE);
            return FALSE;
    }
}

/******************************************************************************
 * CPSetKeyParam (RSAENH.@)
 *
 * Set a parameter of a key object
 *
 * PARAMS
 *  hProv   [I] The key container to which the key belongs.
 *  hKey    [I] The key for which a parameter is to be set.
 *  dwParam [I] Parameter type. See Notes.
 *  pbData  [I] Pointer to the parameter value.
 *  dwFlags [I] Currently none defined.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * NOTES:
 *  Defined dwParam types are:
 *   - KP_MODE: Values MODE_CBC, MODE_ECB, MODE_CFB.
 *   - KP_MODE_BITS: Shift width for cipher feedback mode. (Currently ignored by MS CSP's)
 *   - KP_PERMISSIONS: Or'ed combination of CRYPT_ENCRYPT, CRYPT_DECRYPT, 
 *                     CRYPT_EXPORT, CRYPT_READ, CRYPT_WRITE, CRYPT_MAC
 *   - KP_IV: Initialization vector
 */
BOOL WINAPI RSAENH_CPSetKeyParam(HCRYPTPROV hProv, HCRYPTKEY hKey, DWORD dwParam, BYTE *pbData, 
                                 DWORD dwFlags)
{
    CRYPTKEY *pCryptKey;

    TRACE("(hProv=%08lx, hKey=%08lx, dwParam=%08lx, pbData=%p, dwFlags=%08lx)\n", hProv, hKey, 
          dwParam, pbData, dwFlags);

    if (!is_valid_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    
    if (!lookup_handle(&handle_table, (unsigned int)hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }
    
    switch (dwParam) {
        case KP_MODE:
            pCryptKey->dwMode = *(DWORD*)pbData;
            return TRUE;

        case KP_MODE_BITS:
            pCryptKey->dwModeBits = *(DWORD*)pbData;
            return TRUE;

        case KP_PERMISSIONS:
            pCryptKey->dwPermissions = *(DWORD*)pbData;
            return TRUE;

        case KP_IV:
            memcpy(pCryptKey->abInitVector, pbData, pCryptKey->dwBlockLen);
            return TRUE;

        case KP_SCHANNEL_ALG:
            switch (((PSCHANNEL_ALG)pbData)->dwUse) {
                case SCHANNEL_ENC_KEY:
                    memcpy(&pCryptKey->siSChannelInfo.saEncAlg, pbData, sizeof(SCHANNEL_ALG));
                    break;

                case SCHANNEL_MAC_KEY:
                    memcpy(&pCryptKey->siSChannelInfo.saMACAlg, pbData, sizeof(SCHANNEL_ALG));
                    break;

                default:
                    SetLastError(NTE_FAIL); /* FIXME: error code */
                    return FALSE;
            }
            return TRUE;

        case KP_CLIENT_RANDOM:
            return copy_data_blob(&pCryptKey->siSChannelInfo.blobClientRandom, (PCRYPT_DATA_BLOB)pbData);
            
        case KP_SERVER_RANDOM:
            return copy_data_blob(&pCryptKey->siSChannelInfo.blobServerRandom, (PCRYPT_DATA_BLOB)pbData);

        default:
            SetLastError(NTE_BAD_TYPE);
            return FALSE;
    }
}

/******************************************************************************
 * CPGetKeyParam (RSAENH.@)
 *
 * Query a key parameter.
 *
 * PARAMS
 *  hProv      [I]   The key container, which the key belongs to.
 *  hHash      [I]   The key object that is to be queried.
 *  dwParam    [I]   Specifies the parameter that is to be queried.
 *  pbData     [I]   Pointer to the buffer where the parameter value will be stored.
 *  pdwDataLen [I/O] I: Buffer length at pbData, O: Length of the parameter value.
 *  dwFlags    [I]   None currently defined.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 *
 * NOTES
 *  Defined dwParam types are:
 *   - KP_MODE: Values MODE_CBC, MODE_ECB, MODE_CFB.
 *   - KP_MODE_BITS: Shift width for cipher feedback mode. 
 *                   (Currently ignored by MS CSP's - always eight)
 *   - KP_PERMISSIONS: Or'ed combination of CRYPT_ENCRYPT, CRYPT_DECRYPT, 
 *                     CRYPT_EXPORT, CRYPT_READ, CRYPT_WRITE, CRYPT_MAC
 *   - KP_IV: Initialization vector.
 *   - KP_KEYLEN: Bitwidth of the key.
 *   - KP_BLOCKLEN: Size of a block cipher block.
 *   - KP_SALT: Salt value.
 */
BOOL WINAPI RSAENH_CPGetKeyParam(HCRYPTPROV hProv, HCRYPTKEY hKey, DWORD dwParam, BYTE *pbData, 
                                 DWORD *pdwDataLen, DWORD dwFlags)
{
    CRYPTKEY *pCryptKey;
    DWORD dwBitLen;
        
    TRACE("(hProv=%08lx, hKey=%08lx, dwParam=%08lx, pbData=%p, pdwDataLen=%p dwFlags=%08lx)\n", 
          hProv, hKey, dwParam, pbData, pdwDataLen, dwFlags);

    if (!is_valid_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER)) 
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, (unsigned int)hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    switch (dwParam) 
    {
        case KP_IV:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)pCryptKey->abInitVector, 
                              pCryptKey->dwBlockLen);
        
        case KP_SALT:
            return copy_param(pbData, pdwDataLen, 
                    (CONST BYTE*)&pCryptKey->abKeyValue[pCryptKey->dwKeyLen], pCryptKey->dwSaltLen);
        
        case KP_KEYLEN:
            dwBitLen = pCryptKey->dwKeyLen << 3;
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&dwBitLen, sizeof(DWORD));
        
        case KP_BLOCKLEN:
            dwBitLen = pCryptKey->dwBlockLen << 3;
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&dwBitLen, sizeof(DWORD));
    
        case KP_MODE:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&pCryptKey->dwMode, sizeof(DWORD));

        case KP_MODE_BITS:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&pCryptKey->dwModeBits, 
                              sizeof(DWORD));
    
        case KP_PERMISSIONS:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&pCryptKey->dwPermissions, 
                              sizeof(DWORD));

        case KP_ALGID:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&pCryptKey->aiAlgid, sizeof(DWORD));
            
        default:
            SetLastError(NTE_BAD_TYPE);
            return FALSE;
    }
}
                        
/******************************************************************************
 * CPGetProvParam (RSAENH.@)
 *
 * Query a CSP parameter.
 *
 * PARAMS
 *  hProv      [I]   The key container that is to be queried.
 *  dwParam    [I]   Specifies the parameter that is to be queried.
 *  pbData     [I]   Pointer to the buffer where the parameter value will be stored.
 *  pdwDataLen [I/O] I: Buffer length at pbData, O: Length of the parameter value.
 *  dwFlags    [I]   CRYPT_FIRST: Start enumeration (for PP_ENUMALGS{_EX}).
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 * NOTES:
 *  Defined dwParam types:
 *   - PP_CONTAINER: Name of the key container.
 *   - PP_NAME: Name of the cryptographic service provider.
 *   - PP_SIG_KEYSIZE_INC: RSA signature keywidth granularity in bits.
 *   - PP_KEYX_KEYSIZE_INC: RSA key-exchange keywidth granularity in bits.
 *   - PP_ENUMALGS{_EX}: Query provider capabilities.
 */
BOOL WINAPI RSAENH_CPGetProvParam(HCRYPTPROV hProv, DWORD dwParam, BYTE *pbData, 
                                  DWORD *pdwDataLen, DWORD dwFlags)
{
    KEYCONTAINER *pKeyContainer;
    PROV_ENUMALGS provEnumalgs;
    DWORD dwTemp;
    CHAR szRSABase[MAX_PATH];
    HKEY hKey, hRootKey;
   
    /* This is for dwParam 41, which does not seem to be documented
     * on MSDN. IE6 SP1 asks for it in the 'About' dialog, however.
     * Returning this BLOB seems to satisfy IE. The marked 0x00 seem 
     * to be 'don't care's. If you know anything more specific about
     * provider parameter 41, please report to wine-devel@winehq.org */
    static CONST BYTE abWTF[96] = { 
        0xb0, 0x25,     0x63,     0x86, 0x9c, 0xab,     0xb6,     0x37, 
        0xe8, 0x82, /**/0x00,/**/ 0x72, 0x06, 0xb2, /**/0x00,/**/ 0x3b, 
        0x60, 0x35, /**/0x00,/**/ 0x3b, 0x88, 0xce, /**/0x00,/**/ 0x82, 
        0xbc, 0x7a, /**/0x00,/**/ 0xb7, 0x4f, 0x7e, /**/0x00,/**/ 0xde, 
        0x92, 0xf1, /**/0x00,/**/ 0x83, 0xea, 0x5e, /**/0x00,/**/ 0xc8, 
        0x12, 0x1e,     0xd4,     0x06, 0xf7, 0x66, /**/0x00,/**/ 0x01, 
        0x29, 0xa4, /**/0x00,/**/ 0xf8, 0x24, 0x0c, /**/0x00,/**/ 0x33, 
        0x06, 0x80, /**/0x00,/**/ 0x02, 0x46, 0x0b, /**/0x00,/**/ 0x6d, 
        0x5b, 0xca, /**/0x00,/**/ 0x9a, 0x10, 0xf0, /**/0x00,/**/ 0x05, 
        0x19, 0xd0, /**/0x00,/**/ 0x2c, 0xf6, 0x27, /**/0x00,/**/ 0xaa, 
        0x7c, 0x6f, /**/0x00,/**/ 0xb9, 0xd8, 0x72, /**/0x00,/**/ 0x03, 
        0xf3, 0x81, /**/0x00,/**/ 0xfa, 0xe8, 0x26, /**/0x00,/**/ 0xca 
    };

    TRACE("(hProv=%08lx, dwParam=%08lx, pbData=%p, pdwDataLen=%p, dwFlags=%08lx)\n", 
           hProv, dwParam, pbData, pdwDataLen, dwFlags);

    if (!pdwDataLen) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    
    if (!lookup_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER, 
                       (OBJECTHDR**)&pKeyContainer)) 
    {
        /* MSDN: hProv not containing valid context handle */
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    switch (dwParam) 
    {
        case PP_CONTAINER:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)pKeyContainer->szName, 
                              strlen(pKeyContainer->szName)+1);

        case PP_NAME:
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)pKeyContainer->szProvName, 
                              strlen(pKeyContainer->szProvName)+1);

        case PP_SIG_KEYSIZE_INC:
        case PP_KEYX_KEYSIZE_INC:
            dwTemp = 8;
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&dwTemp, sizeof(dwTemp));

        case PP_IMPTYPE:
            dwTemp = CRYPT_IMPL_SOFTWARE;
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&dwTemp, sizeof(dwTemp));

        case PP_VERSION:
            dwTemp = 0x00000200;
            return copy_param(pbData, pdwDataLen, (CONST BYTE*)&dwTemp, sizeof(dwTemp));
            
        case PP_ENUMCONTAINERS:
            if ((dwFlags & CRYPT_FIRST) == CRYPT_FIRST) pKeyContainer->dwEnumContainersCtr = 0;

            if (!pbData) {
                *pdwDataLen = (DWORD)MAX_PATH + 1;
                return TRUE;
            }
 
            sprintf(szRSABase, RSAENH_REGKEY, "");

            if (dwFlags & CRYPT_MACHINE_KEYSET) {
                hRootKey = HKEY_LOCAL_MACHINE;
            } else {
                hRootKey = HKEY_CURRENT_USER;
            }

            if (RegOpenKeyExA(hRootKey, szRSABase, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
            {
                SetLastError(ERROR_NO_MORE_ITEMS);
                return FALSE;
            }

            dwTemp = *pdwDataLen;
            switch (RegEnumKeyExA(hKey, pKeyContainer->dwEnumContainersCtr, (LPSTR)pbData, &dwTemp,
                    NULL, NULL, NULL, NULL))
            {
                case ERROR_MORE_DATA:
                    *pdwDataLen = (DWORD)MAX_PATH + 1;
 
                case ERROR_SUCCESS:
                    pKeyContainer->dwEnumContainersCtr++;
                    RegCloseKey(hKey);
                    return TRUE;

                case ERROR_NO_MORE_ITEMS:
                default:
                    SetLastError(ERROR_NO_MORE_ITEMS);
                    RegCloseKey(hKey);
                    return FALSE;
            }
 
        case PP_ENUMALGS:
        case PP_ENUMALGS_EX:
            if (((pKeyContainer->dwEnumAlgsCtr >= RSAENH_MAX_ENUMALGS-1) ||
                 (!aProvEnumAlgsEx[pKeyContainer->dwPersonality]
                   [pKeyContainer->dwEnumAlgsCtr+1].aiAlgid)) && 
                ((dwFlags & CRYPT_FIRST) != CRYPT_FIRST))
            {
                SetLastError(ERROR_NO_MORE_ITEMS);
                return FALSE;
            }

            if (dwParam == PP_ENUMALGS) {    
                if (pbData && (*pdwDataLen >= sizeof(PROV_ENUMALGS))) 
                    pKeyContainer->dwEnumAlgsCtr = ((dwFlags & CRYPT_FIRST) == CRYPT_FIRST) ? 
                        0 : pKeyContainer->dwEnumAlgsCtr+1;
            
                provEnumalgs.aiAlgid = aProvEnumAlgsEx
                    [pKeyContainer->dwPersonality][pKeyContainer->dwEnumAlgsCtr].aiAlgid;
                provEnumalgs.dwBitLen = aProvEnumAlgsEx
                    [pKeyContainer->dwPersonality][pKeyContainer->dwEnumAlgsCtr].dwDefaultLen;
                provEnumalgs.dwNameLen = aProvEnumAlgsEx
                    [pKeyContainer->dwPersonality][pKeyContainer->dwEnumAlgsCtr].dwNameLen;
                memcpy(provEnumalgs.szName, aProvEnumAlgsEx
                       [pKeyContainer->dwPersonality][pKeyContainer->dwEnumAlgsCtr].szName, 
                       20*sizeof(CHAR));
            
                return copy_param(pbData, pdwDataLen, (CONST BYTE*)&provEnumalgs, 
                                  sizeof(PROV_ENUMALGS));
            } else {
                if (pbData && (*pdwDataLen >= sizeof(PROV_ENUMALGS_EX))) 
                    pKeyContainer->dwEnumAlgsCtr = ((dwFlags & CRYPT_FIRST) == CRYPT_FIRST) ? 
                        0 : pKeyContainer->dwEnumAlgsCtr+1;
            
                return copy_param(pbData, pdwDataLen, 
                                  (CONST BYTE*)&aProvEnumAlgsEx
                                      [pKeyContainer->dwPersonality][pKeyContainer->dwEnumAlgsCtr], 
                                  sizeof(PROV_ENUMALGS_EX));
            }

        case 41: /* Undocumented. Asked for by IE About dialog */
            return copy_param(pbData, pdwDataLen, abWTF, sizeof(abWTF));

        default:
            /* MSDN: Unknown parameter number in dwParam */
            SetLastError(NTE_BAD_TYPE);
            return FALSE;
    }
}

/******************************************************************************
 * CPDeriveKey (RSAENH.@)
 *
 * Derives a key from a hash value.
 *
 * PARAMS
 *  hProv     [I] Key container for which a key is to be generated.
 *  Algid     [I] Crypto algorithm identifier for the key to be generated.
 *  hBaseData [I] Hash from whose value the key will be derived.
 *  dwFlags   [I] See Notes.
 *  phKey     [O] The generated key.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 *
 * NOTES
 *  Defined flags:
 *   - CRYPT_EXPORTABLE: Key can be exported.
 *   - CRYPT_NO_SALT: No salt is used for 40 bit keys.
 *   - CRYPT_CREATE_SALT: Use remaining bits as salt value.
 */
BOOL WINAPI RSAENH_CPDeriveKey(HCRYPTPROV hProv, ALG_ID Algid, HCRYPTHASH hBaseData, 
                               DWORD dwFlags, HCRYPTKEY *phKey)
{
    CRYPTKEY *pCryptKey, *pMasterKey;
    CRYPTHASH *pCryptHash;
    BYTE abHashValue[RSAENH_MAX_HASH_SIZE*2];
    DWORD dwLen;
    
    TRACE("(hProv=%08lx, Algid=%d, hBaseData=%08lx, dwFlags=%08lx phKey=%p)\n", hProv, Algid, 
           hBaseData, dwFlags, phKey);
    
    if (!is_valid_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, (unsigned int)hBaseData, RSAENH_MAGIC_HASH, 
                       (OBJECTHDR**)&pCryptHash))
    {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }

    if (!phKey)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (GET_ALG_CLASS(Algid))
    {
        case ALG_CLASS_DATA_ENCRYPT:
            *phKey = new_key(hProv, Algid, dwFlags, &pCryptKey);
            if (*phKey == (HCRYPTKEY)INVALID_HANDLE_VALUE) return FALSE;

            /* 
             * We derive the key material from the hash.
             * If the hash value is not large enough for the claimed key, we have to construct
             * a larger binary value based on the hash. This is documented in MSDN: CryptDeriveKey.
             */
            dwLen = RSAENH_MAX_HASH_SIZE;
            RSAENH_CPGetHashParam(pCryptHash->hProv, hBaseData, HP_HASHVAL, abHashValue, &dwLen, 0);
    
            if (dwLen < pCryptKey->dwKeyLen) {
                BYTE pad1[RSAENH_HMAC_DEF_PAD_LEN], pad2[RSAENH_HMAC_DEF_PAD_LEN];
                BYTE old_hashval[RSAENH_MAX_HASH_SIZE];
                DWORD i;

                memcpy(old_hashval, pCryptHash->abHashValue, RSAENH_MAX_HASH_SIZE);
            
                for (i=0; i<RSAENH_HMAC_DEF_PAD_LEN; i++) {
                    pad1[i] = RSAENH_HMAC_DEF_IPAD_CHAR ^ (i<dwLen ? abHashValue[i] : 0);
                    pad2[i] = RSAENH_HMAC_DEF_OPAD_CHAR ^ (i<dwLen ? abHashValue[i] : 0);
                }
                
                init_hash(pCryptHash);
                update_hash(pCryptHash, pad1, RSAENH_HMAC_DEF_PAD_LEN);
                finalize_hash(pCryptHash);
                memcpy(abHashValue, pCryptHash->abHashValue, pCryptHash->dwHashSize);

                init_hash(pCryptHash);
                update_hash(pCryptHash, pad2, RSAENH_HMAC_DEF_PAD_LEN);
                finalize_hash(pCryptHash);
                memcpy(abHashValue+pCryptHash->dwHashSize, pCryptHash->abHashValue, 
                       pCryptHash->dwHashSize);

                memcpy(pCryptHash->abHashValue, old_hashval, RSAENH_MAX_HASH_SIZE);
            }
    
            memcpy(pCryptKey->abKeyValue, abHashValue, 
                   RSAENH_MIN(pCryptKey->dwKeyLen, sizeof(pCryptKey->abKeyValue)));
            break;

        case ALG_CLASS_MSG_ENCRYPT:
            if (!lookup_handle(&handle_table, pCryptHash->hKey, RSAENH_MAGIC_KEY,
                               (OBJECTHDR**)&pMasterKey)) 
            {
                SetLastError(NTE_FAIL); /* FIXME error code */
                return FALSE;
            }
                
            switch (Algid) 
            {
                /* See RFC 2246, chapter 6.3 Key calculation */
                case CALG_SCHANNEL_ENC_KEY:
                    *phKey = new_key(hProv, pMasterKey->siSChannelInfo.saEncAlg.Algid, 
                                     MAKELONG(LOWORD(dwFlags),pMasterKey->siSChannelInfo.saEncAlg.cBits),
                                     &pCryptKey);
                    if (*phKey == (HCRYPTKEY)INVALID_HANDLE_VALUE) return FALSE;
                    memcpy(pCryptKey->abKeyValue, 
                           pCryptHash->abHashValue + (
                               2 * (pMasterKey->siSChannelInfo.saMACAlg.cBits / 8) +
                               ((dwFlags & CRYPT_SERVER) ? 
                                   (pMasterKey->siSChannelInfo.saEncAlg.cBits / 8) : 0)),
                           pMasterKey->siSChannelInfo.saEncAlg.cBits / 8);
                    memcpy(pCryptKey->abInitVector,
                           pCryptHash->abHashValue + (
                               2 * (pMasterKey->siSChannelInfo.saMACAlg.cBits / 8) +
                               2 * (pMasterKey->siSChannelInfo.saEncAlg.cBits / 8) +
                               ((dwFlags & CRYPT_SERVER) ? pCryptKey->dwBlockLen : 0)),
                           pCryptKey->dwBlockLen);
                    break;
                    
                case CALG_SCHANNEL_MAC_KEY:
                    *phKey = new_key(hProv, Algid, 
                                     MAKELONG(LOWORD(dwFlags),pMasterKey->siSChannelInfo.saMACAlg.cBits),
                                     &pCryptKey);
                    if (*phKey == (HCRYPTKEY)INVALID_HANDLE_VALUE) return FALSE;
                    memcpy(pCryptKey->abKeyValue,
                           pCryptHash->abHashValue + ((dwFlags & CRYPT_SERVER) ? 
                               pMasterKey->siSChannelInfo.saMACAlg.cBits / 8 : 0),
                           pMasterKey->siSChannelInfo.saMACAlg.cBits / 8);
                    break;
                    
                default:
                    SetLastError(NTE_BAD_ALGID);
                    return FALSE;
            }
            break;

        default:
            SetLastError(NTE_BAD_ALGID);
            return FALSE;
    }

    setup_key(pCryptKey);
    return TRUE;    
}

/******************************************************************************
 * CPGetUserKey (RSAENH.@)
 *
 * Returns a handle to the user's private key-exchange- or signature-key.
 *
 * PARAMS
 *  hProv     [I] The key container from which a user key is requested.
 *  dwKeySpec [I] AT_KEYEXCHANGE or AT_SIGNATURE
 *  phUserKey [O] Handle to the requested key or INVALID_HANDLE_VALUE in case of failure.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * NOTE
 *  A newly created key container does not contain private user key. Create them with CPGenKey.
 */
BOOL WINAPI RSAENH_CPGetUserKey(HCRYPTPROV hProv, DWORD dwKeySpec, HCRYPTKEY *phUserKey)
{
    KEYCONTAINER *pKeyContainer;

    TRACE("(hProv=%08lx, dwKeySpec=%08lx, phUserKey=%p)\n", hProv, dwKeySpec, phUserKey);
    
    if (!lookup_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER, 
                       (OBJECTHDR**)&pKeyContainer)) 
    {
        /* MSDN: hProv not containing valid context handle */
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    switch (dwKeySpec)
    {
        case AT_KEYEXCHANGE:
            copy_handle(&handle_table, pKeyContainer->hKeyExchangeKeyPair, RSAENH_MAGIC_KEY, 
                        (unsigned int*)phUserKey);
            break;

        case AT_SIGNATURE:
            copy_handle(&handle_table, pKeyContainer->hSignatureKeyPair, RSAENH_MAGIC_KEY, 
                        (unsigned int*)phUserKey);
            break;

        default:
            *phUserKey = (HCRYPTKEY)INVALID_HANDLE_VALUE;
    }

    if (*phUserKey == (HCRYPTKEY)INVALID_HANDLE_VALUE)
    {
        /* MSDN: dwKeySpec parameter specifies nonexistent key */
        SetLastError(NTE_NO_KEY);
        return FALSE;
    }

    return TRUE;
}

/******************************************************************************
 * CPHashData (RSAENH.@)
 *
 * Updates a hash object with the given data.
 *
 * PARAMS
 *  hProv     [I] Key container to which the hash object belongs.
 *  hHash     [I] Hash object which is to be updated.
 *  pbData    [I] Pointer to data with which the hash object is to be updated.
 *  dwDataLen [I] Length of the data.
 *  dwFlags   [I] Currently none defined.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * NOTES
 *  The actual hash value is queried with CPGetHashParam, which will finalize 
 *  the hash. Updating a finalized hash will fail with a last error NTE_BAD_HASH_STATE.
 */
BOOL WINAPI RSAENH_CPHashData(HCRYPTPROV hProv, HCRYPTHASH hHash, CONST BYTE *pbData, 
                              DWORD dwDataLen, DWORD dwFlags)
{
    CRYPTHASH *pCryptHash;
        
    TRACE("(hProv=%08lx, hHash=%08lx, pbData=%p, dwDataLen=%ld, dwFlags=%08lx)\n", 
          hProv, hHash, pbData, dwDataLen, dwFlags);

    if (dwFlags)
    {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    if (!lookup_handle(&handle_table, (unsigned int)hHash, RSAENH_MAGIC_HASH, 
                       (OBJECTHDR**)&pCryptHash))
    {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }

    if (!get_algid_info(hProv, pCryptHash->aiAlgid) || pCryptHash->aiAlgid == CALG_SSL3_SHAMD5)
    {
        SetLastError(NTE_BAD_ALGID);
        return FALSE;
    }
    
    if (pCryptHash->dwState == RSAENH_HASHSTATE_IDLE)
        pCryptHash->dwState = RSAENH_HASHSTATE_HASHING;
    
    if (pCryptHash->dwState != RSAENH_HASHSTATE_HASHING)
    {
        SetLastError(NTE_BAD_HASH_STATE);
        return FALSE;
    }

    update_hash(pCryptHash, pbData, dwDataLen);
    return TRUE;
}

/******************************************************************************
 * CPHashSessionKey (RSAENH.@)
 *
 * Updates a hash object with the binary representation of a symmetric key.
 *
 * PARAMS
 *  hProv     [I] Key container to which the hash object belongs.
 *  hHash     [I] Hash object which is to be updated.
 *  hKey      [I] The symmetric key, whose binary value will be added to the hash.
 *  dwFlags   [I] CRYPT_LITTLE_ENDIAN, if the binary key value shall be interpreted as little endian.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 */
BOOL WINAPI RSAENH_CPHashSessionKey(HCRYPTPROV hProv, HCRYPTHASH hHash, HCRYPTKEY hKey, 
                                    DWORD dwFlags)
{
    BYTE abKeyValue[RSAENH_MAX_KEY_SIZE], bTemp;
    CRYPTKEY *pKey;
    DWORD i;

    TRACE("(hProv=%08lx, hHash=%08lx, hKey=%08lx, dwFlags=%08lx)\n", hProv, hHash, hKey, dwFlags);

    if (!lookup_handle(&handle_table, (unsigned int)hKey, RSAENH_MAGIC_KEY, (OBJECTHDR**)&pKey) ||
        (GET_ALG_CLASS(pKey->aiAlgid) != ALG_CLASS_DATA_ENCRYPT)) 
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    if (dwFlags & ~CRYPT_LITTLE_ENDIAN) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }

    memcpy(abKeyValue, pKey->abKeyValue, pKey->dwKeyLen);
    if (!(dwFlags & CRYPT_LITTLE_ENDIAN)) {
        for (i=0; i<pKey->dwKeyLen/2; i++) {
            bTemp = abKeyValue[i];
            abKeyValue[i] = abKeyValue[pKey->dwKeyLen-i-1];
            abKeyValue[pKey->dwKeyLen-i-1] = bTemp;
        }
    }

    return RSAENH_CPHashData(hProv, hHash, abKeyValue, pKey->dwKeyLen, 0);
}

/******************************************************************************
 * CPReleaseContext (RSAENH.@)
 *
 * Release a key container.
 *
 * PARAMS
 *  hProv   [I] Key container to be released.
 *  dwFlags [I] Currently none defined.
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 */
BOOL WINAPI RSAENH_CPReleaseContext(HCRYPTPROV hProv, DWORD dwFlags)
{
    TRACE("(hProv=%08lx, dwFlags=%08lx)\n", hProv, dwFlags);

    if (!release_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER)) 
    {
        /* MSDN: hProv not containing valid context handle */
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    
    return TRUE;
}

/******************************************************************************
 * CPSetHashParam (RSAENH.@)
 * 
 * Set a parameter of a hash object
 *
 * PARAMS
 *  hProv   [I] The key container to which the key belongs.
 *  hHash   [I] The hash object for which a parameter is to be set.
 *  dwParam [I] Parameter type. See Notes.
 *  pbData  [I] Pointer to the parameter value.
 *  dwFlags [I] Currently none defined.
 *
 * RETURNS
 *  Success: TRUE.
 *  Failure: FALSE.
 *
 * NOTES
 *  Currently only the HP_HMAC_INFO dwParam type is defined. 
 *  The HMAC_INFO struct will be deep copied into the hash object.
 *  See Internet RFC 2104 for details on the HMAC algorithm.
 */
BOOL WINAPI RSAENH_CPSetHashParam(HCRYPTPROV hProv, HCRYPTHASH hHash, DWORD dwParam, 
                                  BYTE *pbData, DWORD dwFlags)
{
    CRYPTHASH *pCryptHash;
    CRYPTKEY *pCryptKey;
    int i;

    TRACE("(hProv=%08lx, hHash=%08lx, dwParam=%08lx, pbData=%p, dwFlags=%08lx)\n", 
           hProv, hHash, dwParam, pbData, dwFlags);

    if (!is_valid_handle(&handle_table, (unsigned int)hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }

    if (dwFlags) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    
    if (!lookup_handle(&handle_table, (unsigned int)hHash, RSAENH_MAGIC_HASH, 
                       (OBJECTHDR**)&pCryptHash))
    {
        SetLastError(NTE_BAD_HASH);
        return FALSE;
    }
    
    switch (dwParam) {
        case HP_HMAC_INFO:
            free_hmac_info(pCryptHash->pHMACInfo);
            if (!copy_hmac_info(&pCryptHash->pHMACInfo, (PHMAC_INFO)pbData)) return FALSE;

            if (!lookup_handle(&handle_table, pCryptHash->hKey, RSAENH_MAGIC_KEY, 
                               (OBJECTHDR**)&pCryptKey)) 
            {
                SetLastError(NTE_FAIL); /* FIXME: correct error code? */
                return FALSE;
            }

            for (i=0; i<RSAENH_MIN(pCryptKey->dwKeyLen,pCryptHash->pHMACInfo->cbInnerString); i++) {
                pCryptHash->pHMACInfo->pbInnerString[i] ^= pCryptKey->abKeyValue[i];
            }
            for (i=0; i<RSAENH_MIN(pCryptKey->dwKeyLen,pCryptHash->pHMACInfo->cbOuterString); i++) {
                pCryptHash->pHMACInfo->pbOuterString[i] ^= pCryptKey->abKeyValue[i];
            }
            
            init_hash(pCryptHash);
            return TRUE;

        case HP_HASHVAL:
            memcpy(pCryptHash->abHashValue, pbData, pCryptHash->dwHashSize);
            pCryptHash->dwState = RSAENH_HASHSTATE_FINISHED;
            return TRUE;
           
        case HP_TLS1PRF_SEED:
            return copy_data_blob(&pCryptHash->tpPRFParams.blobSeed, (PCRYPT_DATA_BLOB)pbData);

        case HP_TLS1PRF_LABEL:
            return copy_data_blob(&pCryptHash->tpPRFParams.blobLabel, (PCRYPT_DATA_BLOB)pbData);
            
        default:
            SetLastError(NTE_BAD_TYPE);
            return FALSE;
    }
}

/******************************************************************************
 * CPSetProvParam (RSAENH.@)
 */
BOOL WINAPI RSAENH_CPSetProvParam(HCRYPTPROV hProv, DWORD dwParam, BYTE *pbData, DWORD dwFlags)
{
    FIXME("(stub)\n");
    return FALSE;
}

/******************************************************************************
 * CPSignHash (RSAENH.@)
 *
 * Sign a hash object
 *
 * PARAMS
 *  hProv        [I]   The key container, to which the hash object belongs.
 *  hHash        [I]   The hash object to be signed.
 *  dwKeySpec    [I]   AT_SIGNATURE or AT_KEYEXCHANGE: Key used to generate the signature.
 *  sDescription [I]   Should be NULL for security reasons. 
 *  dwFlags      [I]   0, CRYPT_NOHASHOID or CRYPT_X931_FORMAT: Format of the signature.
 *  pbSignature  [O]   Buffer, to which the signature will be stored. May be NULL to query SigLen.
 *  pdwSigLen    [I/O] Size of the buffer (in), Length of the signature (out)
 *
 * RETURNS
 *  Success: TRUE
 *  Failure: FALSE
 */
BOOL WINAPI RSAENH_CPSignHash(HCRYPTPROV hProv, HCRYPTHASH hHash, DWORD dwKeySpec, 
                              LPCWSTR sDescription, DWORD dwFlags, BYTE *pbSignature, 
                              DWORD *pdwSigLen)
{
    HCRYPTKEY hCryptKey;
    CRYPTKEY *pCryptKey;
    DWORD dwHashLen;
    BYTE abHashValue[RSAENH_MAX_HASH_SIZE];
    ALG_ID aiAlgid;

    TRACE("(hProv=%08lx, hHash=%08lx, dwKeySpec=%08lx, sDescription=%s, dwFlags=%08lx, "
        "pbSignature=%p, pdwSigLen=%p)\n", hProv, hHash, dwKeySpec, debugstr_w(sDescription),
        dwFlags, pbSignature, pdwSigLen);

    if (dwFlags & ~(CRYPT_NOHASHOID|CRYPT_X931_FORMAT)) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    
    if (!RSAENH_CPGetUserKey(hProv, dwKeySpec, &hCryptKey)) return FALSE;
            
    if (!lookup_handle(&handle_table, (unsigned int)hCryptKey, RSAENH_MAGIC_KEY, 
                       (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_NO_KEY);
        return FALSE;
    }

    if (!pbSignature) {
        *pdwSigLen = pCryptKey->dwKeyLen;
        return TRUE;
    }
    if (pCryptKey->dwKeyLen > *pdwSigLen)
    {
        SetLastError(ERROR_MORE_DATA);
        *pdwSigLen = pCryptKey->dwKeyLen;
        return FALSE;
    }
    *pdwSigLen = pCryptKey->dwKeyLen;

    if (sDescription) {
        if (!RSAENH_CPHashData(hProv, hHash, (CONST BYTE*)sDescription, 
                                (DWORD)lstrlenW(sDescription)*sizeof(WCHAR), 0))
        {
            return FALSE;
        }
    }
    
    dwHashLen = sizeof(DWORD);
    if (!RSAENH_CPGetHashParam(hProv, hHash, HP_ALGID, (BYTE*)&aiAlgid, &dwHashLen, 0)) return FALSE;
    
    dwHashLen = RSAENH_MAX_HASH_SIZE;
    if (!RSAENH_CPGetHashParam(hProv, hHash, HP_HASHVAL, abHashValue, &dwHashLen, 0)) return FALSE;
 

    if (!build_hash_signature(pbSignature, *pdwSigLen, aiAlgid, abHashValue, dwHashLen, dwFlags)) {
        return FALSE;
    }

    return encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, pbSignature, pbSignature, RSAENH_ENCRYPT);
}

/******************************************************************************
 * CPVerifySignature (RSAENH.@)
 *
 * Verify the signature of a hash object.
 * 
 * PARAMS
 *  hProv        [I] The key container, to which the hash belongs.
 *  hHash        [I] The hash for which the signature is verified.
 *  pbSignature  [I] The binary signature.
 *  dwSigLen     [I] Length of the signature BLOB.
 *  hPubKey      [I] Public key used to verify the signature.
 *  sDescription [I] Should be NULL for security reasons.
 *  dwFlags      [I] 0, CRYPT_NOHASHOID or CRYPT_X931_FORMAT: Format of the signature.
 *
 * RETURNS
 *  Success: TRUE  (Signature is valid)
 *  Failure: FALSE (GetLastError() == NTE_BAD_SIGNATURE, if signature is invalid)
 */
BOOL WINAPI RSAENH_CPVerifySignature(HCRYPTPROV hProv, HCRYPTHASH hHash, CONST BYTE *pbSignature, 
                                     DWORD dwSigLen, HCRYPTKEY hPubKey, LPCWSTR sDescription, 
                                     DWORD dwFlags)
{
    BYTE *pbConstructed = NULL, *pbDecrypted = NULL;
    CRYPTKEY *pCryptKey;
    DWORD dwHashLen;
    ALG_ID aiAlgid;
    BYTE abHashValue[RSAENH_MAX_HASH_SIZE];
    BOOL res = FALSE;

    TRACE("(hProv=%08lx, hHash=%08lx, pbSignature=%p, dwSigLen=%ld, hPubKey=%08lx, sDescription=%s, "
          "dwFlags=%08lx)\n", hProv, hHash, pbSignature, dwSigLen, hPubKey, debugstr_w(sDescription),
          dwFlags);
        
    if (dwFlags & ~(CRYPT_NOHASHOID|CRYPT_X931_FORMAT)) {
        SetLastError(NTE_BAD_FLAGS);
        return FALSE;
    }
    
    if (!is_valid_handle(&handle_table, hProv, RSAENH_MAGIC_CONTAINER))
    {
        SetLastError(NTE_BAD_UID);
        return FALSE;
    }
 
    if (!lookup_handle(&handle_table, (unsigned int)hPubKey, RSAENH_MAGIC_KEY, 
                       (OBJECTHDR**)&pCryptKey))
    {
        SetLastError(NTE_BAD_KEY);
        return FALSE;
    }

    if (sDescription) {
        if (!RSAENH_CPHashData(hProv, hHash, (CONST BYTE*)sDescription, 
                                (DWORD)lstrlenW(sDescription)*sizeof(WCHAR), 0))
        {
            return FALSE;
        }
    }
    
    dwHashLen = sizeof(DWORD);
    if (!RSAENH_CPGetHashParam(hProv, hHash, HP_ALGID, (BYTE*)&aiAlgid, &dwHashLen, 0)) return FALSE;
    
    dwHashLen = RSAENH_MAX_HASH_SIZE;
    if (!RSAENH_CPGetHashParam(hProv, hHash, HP_HASHVAL, abHashValue, &dwHashLen, 0)) return FALSE;

    pbConstructed = HeapAlloc(GetProcessHeap(), 0, dwSigLen);
    if (!pbConstructed) {
        SetLastError(NTE_NO_MEMORY);
        goto cleanup;
    }

    pbDecrypted = HeapAlloc(GetProcessHeap(), 0, dwSigLen);
    if (!pbDecrypted) {
        SetLastError(NTE_NO_MEMORY);
        goto cleanup;
    }

    if (!encrypt_block_impl(pCryptKey->aiAlgid, &pCryptKey->context, pbSignature, pbDecrypted, 
                            RSAENH_DECRYPT)) 
    {
        goto cleanup;
    }

    if (!build_hash_signature(pbConstructed, dwSigLen, aiAlgid, abHashValue, dwHashLen, dwFlags)) {
        goto cleanup;
    }

    if (memcmp(pbDecrypted, pbConstructed, dwSigLen)) {
        SetLastError(NTE_BAD_SIGNATURE);
        goto cleanup;
    }
    
    res = TRUE;
cleanup:
    HeapFree(GetProcessHeap(), 0, pbConstructed);
    HeapFree(GetProcessHeap(), 0, pbDecrypted);
    return res;
}

static const WCHAR szProviderKeys[4][97] = {
    {   'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\','C','r','y','p','t','o','g','r',
        'a','p','h','y','\\','D','e','f','a','u','l','t','s','\\','P','r','o','v',
        'i','d','e','r','\\','M','i','c','r','o','s','o','f','t',' ','B','a','s',
        'e',' ','C','r','y','p','t','o','g','r','a','p','h','i','c',' ','P','r',
        'o','v','i','d','e','r',' ','v','1','.','0',0 },
    {   'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\','C','r','y','p','t','o','g','r',
        'a','p','h','y','\\','D','e','f','a','u','l','t','s','\\','P','r','o','v',
        'i','d','e','r','\\','M','i','c','r','o','s','o','f','t',' ',
        'E','n','h','a','n','c','e','d',
        ' ','C','r','y','p','t','o','g','r','a','p','h','i','c',' ','P','r',
        'o','v','i','d','e','r',' ','v','1','.','0',0 },
    {   'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\','C','r','y','p','t','o','g','r',
        'a','p','h','y','\\','D','e','f','a','u','l','t','s','\\','P','r','o','v',
        'i','d','e','r','\\','M','i','c','r','o','s','o','f','t',' ','S','t','r','o','n','g',
        ' ','C','r','y','p','t','o','g','r','a','p','h','i','c',' ','P','r',
        'o','v','i','d','e','r',0 },
    {   'S','o','f','t','w','a','r','e','\\','M','i','c','r','o','s','o','f','t','\\',
        'C','r','y','p','t','o','g','r','a','p','h','y','\\','D','e','f','a','u','l','t','s','\\',
        'P','r','o','v','i','d','e','r','\\','M','i','c','r','o','s','o','f','t',' ',
        'R','S','A',' ','S','C','h','a','n','n','e','l',' ',
        'C','r','y','p','t','o','g','r','a','p','h','i','c',' ','P','r','o','v','i','d','e','r',0 }
};
static const WCHAR szDefaultKeys[2][65] = {
    {   'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\','C','r','y','p','t','o','g','r',
        'a','p','h','y','\\','D','e','f','a','u','l','t','s','\\','P','r','o','v',
        'i','d','e','r',' ','T','y','p','e','s','\\','T','y','p','e',' ','0','0','1',0 },
    {   'S','o','f','t','w','a','r','e','\\',
        'M','i','c','r','o','s','o','f','t','\\','C','r','y','p','t','o','g','r',
        'a','p','h','y','\\','D','e','f','a','u','l','t','s','\\','P','r','o','v',
        'i','d','e','r',' ','T','y','p','e','s','\\','T','y','p','e',' ','0','1','2',0 }
};


/******************************************************************************
 * DllRegisterServer (RSAENH.@)
 *
 * Dll self registration. 
 *
 * PARAMS
 *
 * RETURNS
 *  Success: S_OK.
 *    Failure: != S_OK
 * 
 * NOTES
 *  Registers the following keys:
 *   - HKLM\Software\Microsoft\Cryptography\Defaults\Provider\
 *       Microsoft Base Cryptographic Provider v1.0
 *   - HKLM\Software\Microsoft\Cryptography\Defaults\Provider\
 *       Microsoft Enhanced Cryptographic Provider
 *   - HKLM\Software\Microsoft\Cryptography\Defaults\Provider\
 *       Microsoft Strong Cryptographpic Provider
 *   - HKLM\Software\Microsoft\Cryptography\Defaults\Provider Types\Type 001
 */
HRESULT WINAPI DllRegisterServer(void)
{
    HKEY key;
    DWORD dp;
    long apiRet;
    int i;

    for (i=0; i<4; i++) {
        apiRet = RegCreateKeyExW(HKEY_LOCAL_MACHINE, szProviderKeys[i], 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &dp);

        if (apiRet == ERROR_SUCCESS)
        {
            if (dp == REG_CREATED_NEW_KEY)
            {
                static const WCHAR szImagePath[] = { 'I','m','a','g','e',' ','P','a','t','h',0 };
                static const WCHAR szRSABase[] = { 'r','s','a','e','n','h','.','d','l','l',0 };
                static const WCHAR szType[] = { 'T','y','p','e',0 };
                static const WCHAR szSignature[] = { 'S','i','g','n','a','t','u','r','e',0 };
                DWORD type = (i == 3) ? PROV_RSA_SCHANNEL : PROV_RSA_FULL;
                DWORD sign = 0xdeadbeef;
                RegSetValueExW(key, szImagePath, 0, REG_SZ, (LPBYTE)szRSABase, 
                               (lstrlenW(szRSABase) + 1) * sizeof(WCHAR));
                RegSetValueExW(key, szType, 0, REG_DWORD, (LPBYTE)&type, sizeof(type));
                RegSetValueExW(key, szSignature, 0, REG_BINARY, (LPBYTE)&sign, sizeof(sign));
            }
            RegCloseKey(key);
        }
    }
    
    for (i=0; i<2; i++) {
        apiRet = RegCreateKeyExW(HKEY_LOCAL_MACHINE, szDefaultKeys[i], 0, NULL, 
                                 REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &dp);
        if (apiRet == ERROR_SUCCESS)
        {
            if (dp == REG_CREATED_NEW_KEY)
            {
                static const WCHAR szName[] = { 'N','a','m','e',0 };
                static const WCHAR szRSAName[2][46] = {
                  { 'M','i','c','r','o','s','o','f','t',' ', 'B','a','s','e',' ',
                    'C','r','y','p','t','o','g','r','a','p','h','i','c',' ', 
                    'P','r','o','v','i','d','e','r',' ','v','1','.','0',0 },
                  { 'M','i','c','r','o','s','o','f','t',' ','R','S','A',' ',
                    'S','C','h','a','n','n','e','l',' ',
                    'C','r','y','p','t','o','g','r','a','p','h','i','c',' ',
                    'P','r','o','v','i','d','e','r',0 } };
                static const WCHAR szTypeName[] = { 'T','y','p','e','N','a','m','e',0 };
                static const WCHAR szRSATypeName[2][38] = { 
                  { 'R','S','A',' ','F','u','l','l',' ',
                       '(','S','i','g','n','a','t','u','r','e',' ','a','n','d',' ',
                    'K','e','y',' ','E','x','c','h','a','n','g','e',')',0 },
                  { 'R','S','A',' ','S','C','h','a','n','n','e','l',0 } };

                RegSetValueExW(key, szName, 0, REG_SZ, (LPBYTE)szRSAName[i], sizeof(szRSAName));
                RegSetValueExW(key, szTypeName, 0, REG_SZ, 
                                (LPBYTE)szRSATypeName[i],sizeof(szRSATypeName));
            }
        }
        RegCloseKey(key);
    }
    
    return HRESULT_FROM_WIN32(apiRet);
}

/******************************************************************************
 * DllUnregisterServer (RSAENH.@)
 *
 * Dll self unregistration. 
 *
 * PARAMS
 *
 * RETURNS
 *  Success: S_OK
 *
 * NOTES
 *  For the relevant keys see DllRegisterServer.
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, szProviderKeys[0]);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, szProviderKeys[1]);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, szProviderKeys[2]);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, szProviderKeys[3]);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, szDefaultKeys[0]);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, szDefaultKeys[1]);
    return S_OK;
}
