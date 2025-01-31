// Copyright (c) 2021-2022 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.



// This file was originally a raygen shader. But G-buffer decals are drawn
// on primary surfaces, but not in perfect reflections/refractions. Because
// 

// Must be defined:
// - either RAYGEN_PRIMARY_SHADER or RAYGEN_REFL_REFR_SHADER
// - MATERIAL_MAX_ALBEDO_LAYERS

#if defined(RAYGEN_PRIMARY_SHADER) && defined(RAYGEN_REFL_REFR_SHADER)
    #error Only one of RAYGEN_PRIMARY_SHADER and RAYGEN_REFL_REFR_SHADER must be defined
#endif
#if !defined(RAYGEN_PRIMARY_SHADER) && !defined(RAYGEN_REFL_REFR_SHADER)
    #error RAYGEN_PRIMARY_SHADER or RAYGEN_REFL_REFR_SHADER must be defined
#endif
#ifndef MATERIAL_MAX_ALBEDO_LAYERS
    #error MATERIAL_MAX_ALBEDO_LAYERS is not defined
#endif 



#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_CUBEMAPS 7
#define DESC_SET_RENDER_CUBEMAP 8
#include "RaygenCommon.h"

vec3 getRayDir(vec2 inUV)
{
    inUV = inUV * 2.0 - 1.0;
    
    vec4 target = globalUniform.invProjection * vec4(inUV.x, inUV.y, 1, 1);
    vec3 localDir = abs(target.w) < 0.001 ? target.xyz : target.xyz / target.w;   

    vec4 rayDir = globalUniform.invView * vec4(normalize(localDir), 0);
    
    return rayDir.xyz;
}

vec2 getPixelUVWithJitter(const ivec2 pix)
{
    const vec2 pixelCenter = vec2(pix) + vec2(0.5);
    const vec2 jitter = vec2(globalUniform.jitterX, globalUniform.jitterY);

    return (pixelCenter + jitter) / vec2(gl_LaunchSizeEXT.xy);
}

vec3 getRayDirAX(vec2 inUV)
{
    const float AX = 1.0 / globalUniform.renderWidth;
    return getRayDir(inUV + vec2(AX, 0));
}

vec3 getRayDirAY(vec2 inUV)
{
    const float AY = 1.0 / globalUniform.renderHeight;
    return getRayDir(inUV + vec2(0, AY));
}

vec2 getDlssMotionVector(const vec2 motionCurToPrev)
{
    float c = FRONT_FACE_IS_PRIMARY ? 1.0 : -1.0;
    return c * vec2(motionCurToPrev.x * globalUniform.renderWidth, motionCurToPrev.y * globalUniform.renderHeight);
}

vec2 getMotionForInfinitePoint(const ivec2 pix)
{
    // treat as a point with .w=0, i.e. at infinite distance
    vec3 rayDir = getRayDir(getPixelUVWithJitter(pix));

    vec3 viewSpacePosCur   = mat3(globalUniform.view)     * rayDir;
    vec3 viewSpacePosPrev  = mat3(globalUniform.viewPrev) * rayDir;

    vec3 clipSpacePosCur   = mat3(globalUniform.projection)     * viewSpacePosCur;
    vec3 clipSpacePosPrev  = mat3(globalUniform.projectionPrev) * viewSpacePosPrev;

    // don't divide by .w
    vec3 ndcCur            = clipSpacePosCur.xyz;
    vec3 ndcPrev           = clipSpacePosPrev.xyz;

    vec2 screenSpaceCur    = ndcCur.xy  * 0.5 + 0.5;
    vec2 screenSpacePrev   = ndcPrev.xy * 0.5 + 0.5;

    return screenSpacePrev - screenSpaceCur;
}

void storeSky(const ivec2 pix, const vec3 rayDir, bool calculateSkyAndStoreToAlbedo, const vec3 throughput, float firstHitDepthNDC, bool wasSplit)
{
#ifdef RAYGEN_PRIMARY_SHADER
    const bool isPrimaryRay = true;
#else
    const bool isPrimaryRay = false;
#endif

    if (calculateSkyAndStoreToAlbedo)
    {
        imageStoreAlbedoSky(                pix, getSkyPrimary(rayDir), isPrimaryRay);
    }
    else
    {
        // resave sky albedo in a special format
        imageStoreAlbedoSky(                pix, texelFetchAlbedo(pix).rgb, isPrimaryRay);
    }

    vec2 m = getMotionForInfinitePoint(pix);

    imageStoreNormal(                       pix, vec3(0.0));
    imageStoreNormalGeometry(               pix, vec3(0.0));
    imageStore(framebufMetallicRoughness,   pix, vec4(0.0));
    imageStore(framebufDepth,               pix, vec4(MAX_RAY_LENGTH * 2.0, 0.0, 0.0, firstHitDepthNDC));
    imageStore(framebufMotion,              pix, vec4(m, 0.0, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(SURFACE_POSITION_INCORRECT));
    imageStore(framebufVisibilityBuffer,    pix, vec4(UINT32_MAX));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufSectorIndex,         pix, uvec4(SECTOR_INDEX_NONE));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : 0.0));
#ifdef RAYGEN_PRIMARY_SHADER
    imageStore(framebufPrimaryToReflRefr,   pix, uvec4(0));
    imageStore(framebufDepthDlss,           getRegularPixFromCheckerboardPix(pix), vec4(clamp(firstHitDepthNDC, 0.0, 1.0)));
    imageStore(framebufMotionDlss,          getRegularPixFromCheckerboardPix(pix), vec4(getDlssMotionVector(m), 0.0, 0.0));
#endif
}

uint getNewRayMedia(int i, uint prevMedia, uint geometryInstanceFlags)
{
    // if camera is not in vacuum, assume that new media is vacuum
    if (i == 0 && globalUniform.cameraMediaType != MEDIA_TYPE_VACUUM)
    {
       return MEDIA_TYPE_VACUUM;
    }

    return getMediaTypeFromFlags(geometryInstanceFlags);
}

vec3 getWaterNormal(const RayCone rayCone, const vec3 rayDir, const vec3 normalGeom, const vec3 position, bool wasPortal)
{
    const mat3 basis = getONB(normalGeom);
    const vec2 baseUV = vec2(dot(position, basis[0]), dot(position, basis[1])); 


    // how much vertical flow to apply
    float verticality = 1.0 - abs(dot(normalGeom, globalUniform.worldUpVector.xyz));

    // project basis[0] and basis[1] on up vector
    vec2 flowSpeedVertical = 10 * vec2(dot(basis[0], globalUniform.worldUpVector.xyz), 
                                       dot(basis[1], globalUniform.worldUpVector.xyz));

    vec2 flowSpeedHorizontal = vec2(1.0);


    const float uvScale = 0.05 / globalUniform.waterTextureAreaScale;
    vec2 speed0 = uvScale * mix(flowSpeedHorizontal, flowSpeedVertical, verticality) * globalUniform.waterWaveSpeed;
    vec2 speed1 = -0.9 * speed0 * mix(1.0, -0.1, verticality);


    // for texture sampling
    float derivU = globalUniform.waterTextureDerivativesMultiplier * 0.5 * uvScale * getWaterDerivU(rayCone, rayDir, normalGeom);

    // make water sharper if visible through the portal
    if (wasPortal)
    {
        derivU *= 0.1;
    }


    vec2 uv0 = uvScale * baseUV + globalUniform.time * speed0;
    vec3 n0 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv0, derivU).xyz;
    n0.xy = n0.xy * 2.0 - vec2(1.0);


    vec2 uv1 = 0.8 * uvScale * baseUV + globalUniform.time * speed1;
    vec3 n1 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv1, derivU).xyz;
    n1.xy = n1.xy * 2.0 - vec2(1.0);


    vec2 uv2 = 0.1 * (uvScale * baseUV + speed0 * sin(globalUniform.time * 0.5));
    vec3 n2 = getTextureSampleDerivU(globalUniform.waterNormalTextureIndex, uv2, derivU).xyz;
    n2.xy = n2.xy * 2.0 - vec2(1.0);


    const float strength = globalUniform.waterWaveStrength;

    const vec3 n = normalize(vec3(0, 0, 1) + strength * (0.25 * n0 + 0.2 * n1 + 0.1 * n2));
    return basis * n;   
}

bool isBackface(const vec3 normalGeom, const vec3 rayDir)
{
    return dot(normalGeom, -rayDir) < 0.0;
}

vec3 getNormal(const vec3 position, const vec3 normalGeom, const RayCone rayCone, const vec3 rayDir, bool isWater, bool wasPortal)
{
    vec3 normal = normalGeom;

    if (isBackface(normalGeom, rayDir))
    {
        normal *= -1;
    }

    if (isWater)
    {
        normal = getWaterNormal(rayCone, rayDir, normal, position, wasPortal);
    }

    return normal;
}

#ifdef RAYGEN_PRIMARY_SHADER
void main() 
{
    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);

    const vec3 cameraOrigin = globalUniform.cameraPosition.xyz;
    const vec3 cameraRayDir = getRayDir(inUV);
    const vec3 cameraRayDirAX = getRayDirAX(inUV);
    const vec3 cameraRayDirAY = getRayDirAY(inUV);

    const uint randomSeed = getRandomSeed(pix, globalUniform.frameId, globalUniform.renderWidth, globalUniform.renderHeight);
    imageStore(framebufRandomSeed, pix, uvec4(randomSeed));
    
    
    const ShPayload primaryPayload = tracePrimaryRay(cameraOrigin, cameraRayDir);


    const uint currentRayMedia = globalUniform.cameraMediaType;


    // was no hit
    if (!doesPayloadContainHitInfo(primaryPayload))
    {
        vec3 throughput = vec3(1.0);
        throughput *= getMediaTransmittance(currentRayMedia, pow(abs(dot(cameraRayDir, vec3(0,1,0))), -3));

        // if sky is a rasterized geometry, it was already rendered to albedo framebuf 
        storeSky(pix, cameraRayDir, globalUniform.skyType != SKY_TYPE_RASTERIZED_GEOMETRY, vec3(1.0), MAX_RAY_LENGTH * 2.0, false);
        return;
    }


    vec2 motionCurToPrev;
    float motionDepthLinearCurToPrev;
    vec2 gradDepth;
    float firstHitDepthNDC;
    float firstHitDepthLinear;
    float screenEmission;
    const ShHitInfo h = getHitInfoPrimaryRay(primaryPayload, cameraOrigin, cameraRayDirAX, cameraRayDirAY, motionCurToPrev, motionDepthLinearCurToPrev, gradDepth, firstHitDepthNDC, firstHitDepthLinear, screenEmission);


    const bool wasSplit = false;

    vec3 throughput = vec3(1.0);
    throughput *= getMediaTransmittance(currentRayMedia, firstHitDepthLinear);


    imageStoreAlbedoSurface(                pix, h.albedo, screenEmission);
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    // save only the first hit's depth for rasterization, as reflections/refraction only may be losely represented via rasterization
    imageStore(framebufDepth,               pix, vec4(firstHitDepthLinear, gradDepth, firstHitDepthNDC));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(primaryPayload));
    imageStore(framebufViewDirection,       pix, vec4(cameraRayDir, 0.0));
    imageStore(framebufSectorIndex,         pix, uvec4(h.sectorArrayIndex));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : 0.0));

    // save some info for refl/refr shader
    imageStore(framebufPrimaryToReflRefr,   pix, uvec4(h.geometryInstanceFlags, primaryPayload.instIdAndIndex, 0, 0));

    // save info for DLSS, but only about primary surface 
    imageStore(framebufDepthDlss,           getRegularPixFromCheckerboardPix(pix), vec4(clamp(firstHitDepthNDC, 0.0, 1.0)));
    imageStore(framebufMotionDlss,          getRegularPixFromCheckerboardPix(pix), vec4(getDlssMotionVector(motionCurToPrev), 0.0, 0.0));
}
#endif


#ifdef RAYGEN_REFL_REFR_SHADER
void main() 
{
    if (globalUniform.reflectRefractMaxDepth == 0)
    {
        return;
    }


    const ivec2 regularPix = ivec2(gl_LaunchIDEXT.xy);
    const ivec2 pix = getCheckerboardPix(regularPix);
    const vec2 inUV = getPixelUVWithJitter(regularPix);

    const vec3 cameraOrigin = globalUniform.cameraPosition.xyz;
    const vec3 cameraRayDir = getRayDir(inUV);
    const vec3 cameraRayDirAX = getRayDirAX(inUV);
    const vec3 cameraRayDirAY = getRayDirAY(inUV);
    
    const vec4 albedoBuf = texelFetchAlbedo(pix);
    if (isSky(albedoBuf))
    {
        return;
    }



    // restore state from primary shader
    const uvec2 primaryToReflRefrBuf        = texelFetch(framebufPrimaryToReflRefr_Sampler, pix, 0).rg;
    ShHitInfo h;
    h.albedo                                = albedoBuf.rgb;
    h.hitPosition                           = texelFetch(framebufSurfacePosition_Sampler, pix, 0).xyz;
    h.geometryInstanceFlags                 = primaryToReflRefrBuf.r;
    h.normalGeom                            = texelFetchNormalGeometry(pix);
    h.normal                                = texelFetchNormal(pix);
    const vec3  motionBuf                   = texelFetch(framebufMotion_Sampler, pix, 0).rgb;
    vec2        motionCurToPrev             = motionBuf.rg;
    float       motionDepthLinearCurToPrev  = motionBuf.b;
    const vec4  depthBuf                    = texelFetch(framebufDepth_Sampler, pix, 0);
    vec2        gradDepth                   = depthBuf.gb;
    float       firstHitDepthLinear         = depthBuf.r;
    float       firstHitDepthNDC            = depthBuf.a;
    float       screenEmission              = getScreenEmissionFromAlbedo4(albedoBuf);
    vec3        throughput                  = texelFetch(framebufThroughput_Sampler, pix, 0).rgb;
    ShPayload currentPayload;
    currentPayload.instIdAndIndex           = primaryToReflRefrBuf.g;



    RayCone rayCone;
    rayCone.width = 0;
    rayCone.spreadAngle = globalUniform.cameraRayConeSpreadAngle;

    float fullPathLength = firstHitDepthLinear;
    vec3 prevHitPosition = h.hitPosition;
    bool wasSplit = false;
    bool wasPortal = false;
    vec3 virtualPos = h.hitPosition;
    vec3 rayDir = cameraRayDir;
    uint currentRayMedia = globalUniform.cameraMediaType;
    // if there was no hitinfo from refl/refr, preserve primary hitinfo
    bool hitInfoWasOverwritten = false;


    propagateRayCone(rayCone, firstHitDepthLinear);



    for (int i = 0; i < globalUniform.reflectRefractMaxDepth; i++)
    {
        const uint instIndex = unpackInstanceIdAndCustomIndex(currentPayload.instIdAndIndex).y;

        if ((instIndex & INSTANCE_CUSTOM_INDEX_FLAG_REFLECT_REFRACT) == 0)
        {
            break;
        }

        bool isPixOdd = isCheckerboardPixOdd(pix) != 0;


        uint newRayMedia = getNewRayMedia(i, currentRayMedia, h.geometryInstanceFlags);

        bool isPortal = isPortalFromFlags(h.geometryInstanceFlags);
        bool toRefract = isRefractFromFlags(h.geometryInstanceFlags);
        bool toReflect = isReflectFromFlags(h.geometryInstanceFlags);


        if (!toReflect && !toRefract && !isPortal)
        {
            break;
        }


        const float curIndexOfRefraction = getIndexOfRefraction(currentRayMedia);
        const float newIndexOfRefraction = getIndexOfRefraction(newRayMedia);

        const vec3 normal = getNormal(h.hitPosition, h.normalGeom, rayCone, rayDir, !isPortal && (newRayMedia == MEDIA_TYPE_WATER || currentRayMedia == MEDIA_TYPE_WATER), wasPortal);


        bool delaySplitOnNextTime = false;
            
        if ((h.geometryInstanceFlags & GEOM_INST_FLAG_NO_MEDIA_CHANGE) != 0)
        {
            // apply small new media transmittance, and ignore the media (but not the refraction indices)
            throughput *= getMediaTransmittance(newRayMedia, 1.0);
            newRayMedia = currentRayMedia;
            
            // if reflections are disabled if viewing from inside of NO_MEDIA_CHANGE geometry
            delaySplitOnNextTime = (globalUniform.noBackfaceReflForNoMediaChange != 0) && isBackface(h.normalGeom, rayDir);
        }

           

        vec3 rayOrigin = h.hitPosition;
        bool doSplit = !wasSplit;
        bool doRefraction;
        vec3 refractionDir;
        float F;

        if (delaySplitOnNextTime)
        {
            doSplit = false;
            // force refraction for all pixels
            toRefract = true;
            isPixOdd = true;
        }
        
        if (toRefract && calcRefractionDirection(curIndexOfRefraction, newIndexOfRefraction, rayDir, normal, refractionDir))
        {
            doRefraction = isPixOdd;
            F = getFresnelSchlick(curIndexOfRefraction, newIndexOfRefraction, -rayDir, normal);
        }
        else
        {
            // total internal reflection
            doRefraction = false;
            doSplit = false;
            F = 1.0;
        }
        
        if (doRefraction)
        {
            rayDir = refractionDir;
            throughput *= (1 - F);

            // change media
            currentRayMedia = newRayMedia;
        }
        else if (isPortal)
        {
            const mat4x3 portalTransform = transpose(mat3x4(
                globalUniform.portalInputToOutputTransform0, 
                globalUniform.portalInputToOutputTransform1, 
                globalUniform.portalInputToOutputTransform2
            ));
            
            rayDir    = portalTransform * vec4(rayDir, 0.0);
            rayOrigin = portalTransform * vec4(rayOrigin - globalUniform.portalInputPosition.xyz, 1.0) + globalUniform.portalInputPosition.xyz;

            throughput *= h.albedo;

            wasPortal = true;
        }
        else
        {
            rayDir = reflect(rayDir, normal);
            throughput *= F;
        }

        if (doSplit)
        {
            throughput *= 2;
            wasSplit = true;
        }


        if ((h.geometryInstanceFlags & GEOM_INST_FLAG_REFL_REFR_ALBEDO_MULT) != 0)
        {
            throughput *= h.albedo;
        }
        else if ((h.geometryInstanceFlags & GEOM_INST_FLAG_REFL_REFR_ALBEDO_ADD) != 0)
        {
            throughput += h.albedo;
        }


        currentPayload = traceReflectionRefractionRay(rayOrigin, rayDir, instIndex, h.geometryInstanceFlags, doRefraction);

        
        if (!doesPayloadContainHitInfo(currentPayload))
        {
            throughput *= getMediaTransmittance(currentRayMedia, pow(abs(dot(rayDir, globalUniform.worldUpVector.xyz)), -3));

            storeSky(pix, rayDir, true, throughput, firstHitDepthNDC, wasSplit);
            return;  
        }

        float rayLen;
        
        if (doRefraction || isPortal)
        {
            h = getHitInfoWithRayCone_Refraction(
                currentPayload, rayCone, 
                cameraOrigin, rayDir, cameraRayDirAX, cameraRayDirAY,
                prevHitPosition, rayLen,
                motionCurToPrev, motionDepthLinearCurToPrev, 
                gradDepth, 
                screenEmission
            );
        }
        else
        {
            h = getHitInfoWithRayCone_Reflection(
                currentPayload, rayCone, 
                cameraOrigin, rayDir, cameraRayDir, 
                virtualPos, 
                prevHitPosition, rayLen, 
                motionCurToPrev, motionDepthLinearCurToPrev, 
                gradDepth, 
                screenEmission
            );
        }


        hitInfoWasOverwritten = true;
        throughput *= getMediaTransmittance(currentRayMedia, rayLen);
        propagateRayCone(rayCone, rayLen);
        fullPathLength += rayLen;
        prevHitPosition = h.hitPosition;
    }


    if (!hitInfoWasOverwritten)
    {
        return;
    }


    imageStoreAlbedoSurface(                pix, h.albedo, screenEmission);
    imageStoreNormal(                       pix, h.normal);
    imageStoreNormalGeometry(               pix, h.normalGeom);
    imageStore(framebufMetallicRoughness,   pix, vec4(h.metallic, h.roughness, 0, 0));
    // save only the first hit's depth for rasterization, as reflections/refraction only may be losely represented via rasterization
    imageStore(framebufDepth,               pix, vec4(fullPathLength, gradDepth, firstHitDepthNDC));
    imageStore(framebufMotion,              pix, vec4(motionCurToPrev, motionDepthLinearCurToPrev, 0.0));
    imageStore(framebufSurfacePosition,     pix, vec4(h.hitPosition, uintBitsToFloat(h.instCustomIndex)));
    imageStore(framebufVisibilityBuffer,    pix, packVisibilityBuffer(currentPayload));
    imageStore(framebufViewDirection,       pix, vec4(rayDir, 0.0));
    imageStore(framebufSectorIndex,         pix, uvec4(h.sectorArrayIndex));
    imageStore(framebufThroughput,          pix, vec4(throughput, wasSplit ? 1.0 : 0.0));
}
#endif