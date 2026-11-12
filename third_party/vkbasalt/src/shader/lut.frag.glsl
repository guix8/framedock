// The sharpening step is AMD Contrast Adaptive Sharpening (CAS), taken from
// vkBasalt's cas.frag.glsl. Its license:
//
// Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.
// -------
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// -------
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
// -------
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE
#version 450

layout(set=0, binding=0) uniform sampler2D img;
layout(set=1, binding=0) uniform sampler3D lut;
layout(set=2, binding=0) uniform FrameColorParams
{
    // 0 disables sharpening; 1 is CAS at its strongest setting; up to 3 pushes past it (exaggerated).
    float sharpenAmount;
    // 1 while the dashboard's compare button is held: show the original image.
    float bypass;
    // 0: sharpen around the centre of each eye; 1: the whole image; 2: around where each eye looks.
    float sharpenArea;
    // Gaze-following: 0 no point (lens centre), 1 a current point; +2 draws the debug ring.
    float gazeState;
    // Where each eye looks, in its own half of the image (u, v; 0..1, v down).
    float gazeLeftU;
    float gazeLeftV;
    float gazeRightU;
    float gazeRightV;
};

//Only works with cubes not with cuboids
layout(constant_id = 0) const int lutSize = 32;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out vec4 fragColor;

#define textureLod0Offset(img, coord, offset) textureLodOffset(img, coord, 0.0f, offset)
#define textureLod0(img, coord) textureLod(img, coord, 0.0f)

vec3 sharpen(vec3 e, float amount)
{
    // fetch a 3x3 neighborhood around the pixel 'e',
    //  a b c
    //  d(e)f
    //  g h i
    vec3 a = textureLod0Offset(img, textureCoord, ivec2(-1,-1)).rgb;
    vec3 b = textureLod0Offset(img, textureCoord, ivec2( 0,-1)).rgb;
    vec3 c = textureLod0Offset(img, textureCoord, ivec2( 1,-1)).rgb;
    vec3 d = textureLod0Offset(img, textureCoord, ivec2(-1, 0)).rgb;
    vec3 f = textureLod0Offset(img, textureCoord, ivec2( 1, 0)).rgb;
    vec3 g = textureLod0Offset(img, textureCoord, ivec2(-1, 1)).rgb;
    vec3 h = textureLod0Offset(img, textureCoord, ivec2( 0, 1)).rgb;
    vec3 i = textureLod0Offset(img, textureCoord, ivec2( 1, 1)).rgb;

    // Soft min and max.
    //  a b c             b
    //  d e f * 0.5  +  d e f * 0.5
    //  g h i             h
    // These are 2.0x bigger (factored out the extra multiply).

    vec3 mnRGB  = min(min(min(d,e),min(f,b)),h);
    vec3 mnRGB2 = min(min(min(mnRGB,a),min(g,c)),i);
    mnRGB += mnRGB2;

    vec3 mxRGB  = max(max(max(d,e),max(f,b)),h);
    vec3 mxRGB2 = max(max(max(mxRGB,a),max(g,c)),i);
    mxRGB += mxRGB2;

    // Smooth minimum distance to signal limit divided by smooth max.

    // max() keeps black pixels finite; upstream divides by zero there.
    vec3 rcpMxRGB = vec3(1)/max(mxRGB, vec3(1.0 / 65536.0));
    vec3 ampRGB = clamp((min(mnRGB,2.0-mxRGB) * rcpMxRGB),0,1);

    // Shaping amount of sharpening. CAS runs at sharpness 1.0 (peak 5.0);
    // amount blends it with the source so the slider starts at zero, and above 1
    // extrapolates past it.
    ampRGB = inversesqrt(ampRGB);
    float peak = 5.0;
    vec3 wRGB = -vec3(1)/(ampRGB * peak);
    vec3 rcpWeightRGB = vec3(1)/(1.0 + 4.0 * wRGB);

    //                          0 w 0
    //  Filter shape:           w 1 w
    //                          0 w 0

    vec3 window = (b + d) + (f + h);
    vec3 outColor = clamp((window * wRGB + e) * rcpWeightRGB,0,1);

    return clamp(mix(e, outColor, amount), 0.0, 1.0);
}

// By default, sharpen only around the centre of each eye. vrcompositor presents both eyes side
// by side after lens distortion (4320x2160 on the Steam Frame), and the lens edges are blurred
// anyway, so CAS is skipped there. That halves its cost and avoids edge shimmer.
// Distances are relative to half the eye's width: full strength inside centerInner, fading
// out by centerOuter. 2026-11-08: 60% of the earlier radius (0.55/0.80): with the whole earlier area the pass made
// the compositor miss 25-31% of frames while it showed a PC stream; at 60% it missed 1-3% (as with sharpening off),
// and the user saw no difference on text in standalone. The centre is fixed to the lens, not to the gaze.
const float centerInner = 0.33;
const float centerOuter = 0.48;

// Distance from the sharpening centre, in units of half an eye's width (the lens centre, or the gaze point)
float sharpenDistance()
{
    vec2 size = vec2(textureSize(img, 0));
    vec2 eye = textureCoord;
    bool rightEye = false;
    // Two square eyes side by side; any other shape is treated as one eye.
    if (abs(size.x - 2.0 * size.y) < 2.0)
    {
        rightEye = eye.x >= 0.5;
        eye.x = fract(eye.x * 2.0);
    }
    vec2 centre = vec2(0.5);
    if (sharpenArea > 1.5 && mod(gazeState, 2.0) > 0.5)
        centre = rightEye ? vec2(gazeRightU, gazeRightV) : vec2(gazeLeftU, gazeLeftV);
    return length((eye - centre) * 2.0);
}

float sharpenWeight()
{
    if (sharpenArea > 0.5 && sharpenArea < 1.5)
        return 1.0;
    return 1.0 - smoothstep(centerInner, centerOuter, sharpenDistance());
}

void main()
{
    vec4 color = textureLod0(img,textureCoord);
    if (bypass > 0.5)
    {
        fragColor = color;
        return;
    }
    float amount = sharpenAmount > 0.0 ? sharpenAmount * sharpenWeight() : 0.0;
    if (amount > 0.0)
        color.rgb = sharpen(color.rgb, amount);

    //see https://developer.nvidia.com/gpugems/GPUGems2/gpugems2_chapter24.html
    vec3 scale = (vec3(lutSize) - 1.0) / vec3(lutSize);
    vec3 offset = 1.0 / (2.0 * vec3(lutSize));

    fragColor = vec4(textureLod0(lut, scale * color.rgb + offset).rgb, color.a);
    // Debug view (a gaze-debug file next to current.txt, gaze state + 2): the sharpened area itself is tinted, as
    // strongly as it is sharpened (solid inside, fading with the sharpening at its edge). It is drawn from the very
    // weight the sharpening uses, so the tint is where the sharpening is (2026-11-09: a thin ring at the edge of a
    // large area made its centre hard to judge). A small red dot at its centre only with gaze-debug-dot too (+ 4)
    if (gazeState > 1.5 && sharpenAmount > 0.0)  // nothing is sharpened at 0: nothing to show
    {
        fragColor.rgb = mix(fragColor.rgb, vec3(1.0, 0.3, 0.75), 0.3 * sharpenWeight());
        if (gazeState > 3.5 && sharpenDistance() < 0.012)
            fragColor.rgb = vec3(1.0, 0.1, 0.1);
    }
}
