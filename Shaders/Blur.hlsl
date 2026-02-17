// Include common HLSL code.
#include "Shaders/Common.hlsl"

static const int gMaxBlurRadius = 15;

#define N 256
#define CacheSize (N + 2*gMaxBlurRadius)
groupshared float4 gCache[CacheSize];

[numthreads(N, 1, 1)]
void HorzBlurCS(uint3 groupThreadID : SV_GroupThreadID,
                uint3 dispatchThreadID : SV_DispatchThreadID)
{
    Texture2D gInput            = ResourceDescriptorHeap[gBlurInputIndex];
    RWTexture2D<float4> gOutput = ResourceDescriptorHeap[gBlurOutputIndex];

    uint2 imgDims;
    gInput.GetDimensions(imgDims.x, imgDims.y);

    //
    // Fill local thread storage to reduce bandwidth.  To blur 
    // N pixels, we will need to load N + 2*BlurRadius pixels
    // due to the blur radius.
    //
    
    // This thread group runs N threads.  To get the extra 2*BlurRadius pixels, 
    // have 2*BlurRadius threads sample an extra pixel.
    if(groupThreadID.x < gBlurRadius)
    {
        // Clamp out of bound samples that occur at image borders.
        // Note: Need int cast since subtracting.
        int x = max((int)dispatchThreadID.x - gBlurRadius, 0);
        gCache[groupThreadID.x] = gInput[uint2(x, dispatchThreadID.y)];
    }
    if(groupThreadID.x >= N-gBlurRadius)
    {
        // Clamp out of bound samples that occur at image borders.
        int x = min(dispatchThreadID.x + gBlurRadius, imgDims.x-1);
        gCache[groupThreadID.x+2*gBlurRadius] = gInput[uint2(x, dispatchThreadID.y)];
    }

    // Clamp out of bound samples that occur at image borders.
    gCache[groupThreadID.x+gBlurRadius] = gInput[min(dispatchThreadID.xy, imgDims-1)];

    // Wait for all threads to finish.
    GroupMemoryBarrierWithGroupSync();
    
    //
    // Now blur each pixel.
    //

    float4 blurColor = float4(0, 0, 0, 0);
    
    for(int i = -gBlurRadius; i <= gBlurRadius; ++i)
    {
        int k = groupThreadID.x + gBlurRadius + i;
        
        int float4Index = (i+gBlurRadius) / 4;
        int slotIndex = (i+gBlurRadius) & 0x3;
        float weight = gWeightVec[float4Index][slotIndex];
        blurColor += weight*gCache[k];
    }
    
    gOutput[dispatchThreadID.xy] = blurColor;
}

[numthreads(1, N, 1)]
void VertBlurCS(uint3 groupThreadID : SV_GroupThreadID,
                uint3 dispatchThreadID : SV_DispatchThreadID)
{
    Texture2D gInput            = ResourceDescriptorHeap[gBlurInputIndex];
    RWTexture2D<float4> gOutput = ResourceDescriptorHeap[gBlurOutputIndex];

    uint2 imgDims;
    gInput.GetDimensions(imgDims.x, imgDims.y);

    //
    // Fill local thread storage to reduce bandwidth.  To blur 
    // N pixels, we will need to load N + 2*BlurRadius pixels
    // due to the blur radius.
    //
    
    // This thread group runs N threads.  To get the extra 2*BlurRadius pixels, 
    // have 2*BlurRadius threads sample an extra pixel.
    if(groupThreadID.y < gBlurRadius)
    {
        // Clamp out of bound samples that occur at image borders.
        // Note: Need int cast since subtracting.
        int y = max((int)dispatchThreadID.y - gBlurRadius, 0);
        gCache[groupThreadID.y] = gInput[uint2(dispatchThreadID.x, y)];
    }
    if(groupThreadID.y >= N-gBlurRadius)
    {
        // Clamp out of bound samples that occur at image borders.
        int y = min(dispatchThreadID.y + gBlurRadius, imgDims.y-1);
        gCache[groupThreadID.y+2*gBlurRadius] = gInput[uint2(dispatchThreadID.x, y)];
    }
    
    // Clamp out of bound samples that occur at image borders.
    gCache[groupThreadID.y+gBlurRadius] = gInput[min(dispatchThreadID.xy, imgDims-1)];


    // Wait for all threads to finish.
    GroupMemoryBarrierWithGroupSync();
    
    //
    // Now blur each pixel.
    //

    float4 blurColor = float4(0, 0, 0, 0);
    
    for(int i = -gBlurRadius; i <= gBlurRadius; ++i)
    {
        int k = groupThreadID.y + gBlurRadius + i;
        
        int float4Index = (i+gBlurRadius) / 4;
        int slotIndex = (i+gBlurRadius) & 0x3;
        float weight = gWeightVec[float4Index][slotIndex];
        blurColor += weight*gCache[k];
    }
    
    gOutput[dispatchThreadID.xy] = blurColor;
}