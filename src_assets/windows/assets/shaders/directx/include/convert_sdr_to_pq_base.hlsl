#include "include/common.hlsl"

float3 RemoveSRGBCurve(float3 x)
{
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

float3 CONVERT_FUNCTION(float3 input)
{
    float3 rgb = RemoveSRGBCurve(saturate(input));
    rgb = Rec709toRec2020(rgb);
    rgb *= 80;
    return NitsToPQ(rgb);
}
