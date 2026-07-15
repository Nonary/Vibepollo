#include "include/common.hlsl"

cbuffer truehdr_peak_params_cbuffer : register(b1) {
    float truehdr_luminance_scale;
    float3 truehdr_peak_params_padding;
};

float3 trueHDRScRGBTo2100PQ(float3 rgb)
{
    // NVIDIA's public TrueHDR runtime clips its FP16 scRGB output near 1000 nits.
    // The caller compensates the NGX tone curve and supplies the remaining linear
    // luminance expansion here before scRGB is converted to Rec. 2100 PQ.
    return scRGBTo2100PQ(rgb * truehdr_luminance_scale);
}

#define CONVERT_FUNCTION trueHDRScRGBTo2100PQ
