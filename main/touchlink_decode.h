#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t source;
    uint8_t command, delivery, zcl_control;
    const uint8_t *payload;
    size_t size;
} tl_decoded_t;

static inline uint16_t tl_u16(const uint8_t *p)
{ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t tl_u32(const uint8_t *p)
{ return (uint32_t)tl_u16(p) | ((uint32_t)tl_u16(p+2) << 16); }

// Decode only unsecured legacy 802.15.4 Inter-PAN Touchlink frames.
// The PSDU includes its two-byte FCS. Unsupported formats fail closed.
static bool tl_decode(const uint8_t *p, size_t length, tl_decoded_t *out)
{
    if (!p || length < 5) return false;
    size_t end=length-2, pos=3;
    uint16_t fc=tl_u16(p);
    unsigned dst=(fc>>10)&3, src=(fc>>14)&3;
    if ((fc&7)!=1 || (fc&8) || ((fc>>12)&3)>1 || (fc&0x0300) ||
        dst==1 || src==1 || !dst || !src) return false;
    pos += 2 + (dst==3 ? 8 : 2);
    if (!(fc&0x40)) pos+=2;
    unsigned src_size=src==3 ? 8 : 2;
    if (pos+src_size+3 > end) return false;
    uint64_t source=0;
    for (unsigned i=0;i<src_size;i++) source|=(uint64_t)p[pos+i]<<(8*i);
    pos+=src_size;
    if (tl_u16(p+pos)!=0x000b) return false;
    pos+=2;
    uint8_t aps=p[pos++];
    unsigned delivery=(aps>>2)&3;
    if ((aps&3)!=3 || (aps&0xa0) || delivery==1) return false;
    if (delivery==3) pos+=2;
    if (pos+7 > end || tl_u16(p+pos)!=0x1000 || tl_u16(p+pos+2)!=0xc05e) return false;
    pos+=4;
    uint8_t zcl=p[pos++];
    if ((zcl&3)!=1) return false;
    if (zcl&4) pos+=2;
    if (pos+2>end) return false;
    pos++; // ZCL sequence
    out->command=p[pos++];
    out->source=source; out->delivery=delivery; out->zcl_control=zcl;
    out->payload=p+pos; out->size=end-pos;
    return true;
}
