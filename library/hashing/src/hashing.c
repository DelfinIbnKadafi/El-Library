/*
 * hashing - official example plugin for EL.
 *
 * Adds hash functions to EL:
 *
 *   hash_djb2(str):int      djb2 hash (32 bit)
 *   hash_fnv1a(str):int     FNV-1a hash (32 bit)
 *   hash_crc32(str):int     CRC-32 (same value zlib and PNG use)
 *   sha256(str):str         SHA-256 as 64 lowercase hex characters
 *
 * An EL int is a signed 32 bit number, so the three 32 bit hashes are given
 * as the same 32 bits read as signed: a hash above 2147483647 shows up as a
 * negative number (hash - 4294967296). Use sha256 for a text digest.
 *
 * Build (see the Makefile next to this file):
 *   Linux:   gcc -shared -fPIC -O2 -I../../../sdk hashing.c -o ../hashing.so
 *   Windows: gcc -shared -O2 -I../../../sdk hashing.c -o ../hashing.dll
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "elplugin.h"

static const ELApi *api;

/* ---- djb2, FNV-1a, CRC-32 ---- */

static void native_djb2(ELCall *call)
{
    const unsigned char *s = (const unsigned char *)api->GetStr(call, 0);
    uint32_t hash = 5381;

    while (*s)
        hash = hash * 33 + *s++;

    api->ReturnInt(call, (int32_t)hash);
}

static void native_fnv1a(ELCall *call)
{
    const unsigned char *s = (const unsigned char *)api->GetStr(call, 0);
    uint32_t hash = 2166136261u;

    while (*s) {
        hash ^= *s++;
        hash *= 16777619u;
    }

    api->ReturnInt(call, (int32_t)hash);
}

static void native_crc32(ELCall *call)
{
    const unsigned char *s = (const unsigned char *)api->GetStr(call, 0);
    uint32_t crc = 0xFFFFFFFFu;

    while (*s) {
        crc ^= *s++;

        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }

    api->ReturnInt(call, (int32_t)(crc ^ 0xFFFFFFFFu));
}

/* ---- SHA-256 ---- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t state[8], const unsigned char block[64])
{
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }

    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void native_sha256(ELCall *call)
{
    const unsigned char *data = (const unsigned char *)api->GetStr(call, 0);
    size_t length = strlen((const char *)data);

    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    size_t done = 0;

    while (length - done >= 64) {
        sha256_block(state, data + done);
        done += 64;
    }

    /* Last block(s): the rest of the data, a 0x80 byte, zeros, and the
       length in bits as a 64 bit number */
    unsigned char tail[128];
    size_t rest = length - done;

    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + done, rest);
    tail[rest] = 0x80;

    size_t tail_size = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)length * 8;

    for (int i = 0; i < 8; i++)
        tail[tail_size - 1 - i] = (unsigned char)(bits >> (8 * i));

    sha256_block(state, tail);

    if (tail_size == 128)
        sha256_block(state, tail + 64);

    char hex[65];

    for (int i = 0; i < 8; i++)
        snprintf(hex + i * 8, 9, "%08x", (unsigned int)state[i]);

    api->ReturnStr(call, hex);
}

/* ---- plugin entry points ---- */

EL_EXPORT int elp_Supports(void)
{
    return EL_PLUGIN_API_VERSION;
}

EL_EXPORT int elp_Load(const ELApi *a)
{
    api = a;

    api->Register("hash_djb2(str):int", native_djb2);
    api->Register("hash_fnv1a(str):int", native_fnv1a);
    api->Register("hash_crc32(str):int", native_crc32);
    api->Register("sha256(str):str", native_sha256);

    return 0;
}
