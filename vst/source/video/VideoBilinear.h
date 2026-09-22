#pragma once
#include <cstdint>
#include <cstring>
#if defined(__aarch64__) && !defined(SP3CTRA_VIDEO_SCALAR)
#include <arm_neon.h>
#endif
namespace videoblit
{
    inline void bilinearScalar(uint8_t* dst,const uint8_t* a,const uint8_t* b,
                               const uint8_t* c,const uint8_t* d,int wx,int wy)
    {
        for(int k=0;k<4;++k)
        {
            const int top=a[k]+(((b[k]-a[k])*wx+32768)>>16);
            const int bot=c[k]+(((d[k]-c[k])*wx+32768)>>16);
            dst[k]=(uint8_t)(top+(((bot-top)*wy+32768)>>16));
        }
    }
    // Four byte channels together on ARM64. Preserve the reference's three
    // rounded integer lerps exactly (including negative intermediate deltas).
    // Only four bytes are read per tap, including the final texel in a row.
    inline void bilinearPixel(uint8_t* dst,const uint8_t* a,const uint8_t* b,
                              const uint8_t* c,const uint8_t* d,int wx,int wy)
    {
#if defined(__aarch64__) && !defined(SP3CTRA_VIDEO_SCALAR)
        auto load=[](const uint8_t* p) {
            uint32_t word;std::memcpy(&word,p,4);
            return vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(word)))));
        };
        const auto av=load(a),bv=load(b),cv=load(c),dv=load(d);
        const auto half=vdupq_n_s32(32768);
        const auto top=vaddq_s32(av,vshrq_n_s32(vmlaq_n_s32(half,vsubq_s32(bv,av),wx),16));
        const auto bot=vaddq_s32(cv,vshrq_n_s32(vmlaq_n_s32(half,vsubq_s32(dv,cv),wx),16));
        const auto value=vaddq_s32(top,vshrq_n_s32(vmlaq_n_s32(half,vsubq_s32(bot,top),wy),16));
        const auto narrow=vqmovun_s32(value);
        const auto bytes=vqmovn_u16(vcombine_u16(narrow,narrow));
        const uint32_t word=vget_lane_u32(vreinterpret_u32_u8(bytes),0);
        std::memcpy(dst,&word,4);
#else
        bilinearScalar(dst,a,b,c,d,wx,wy);
#endif
    }
}
