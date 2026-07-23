#version 300 es
precision highp float;

in vec2 vTexCoord;

uniform sampler2D uNormalMap;
uniform sampler2D uDepthMap;

uniform vec2 uInvViewportSize;
uniform float uBevelRadius;
uniform float uBevelStrength;
uniform float uNear[2];
uniform float uFar[2];
uniform float uTargetDistance[2];
uniform int uProjType[2];
uniform float uOrthoZoom[2];
uniform float uFov[2];
uniform float uViewportHeight[2];
uniform int uSplitMode;
uniform int uBevelScaleWithDistance;

out vec4 fragColor;

void main() {
    vec2 uv = vTexCoord;
    
    vec3 N0 = texture(uNormalMap, uv).rgb;
    float D0 = texture(uDepthMap, uv).r;
    
    // If background, do not process
    if (D0 >= 1.0) {
        fragColor = vec4(N0, 1.0);
        return;
    }
    
    N0 = N0 * 2.0 - 1.0;
    
    // Dynamic depth threshold using local derivatives
    float dx = dFdx(D0);
    float dy = dFdy(D0);
    
    vec3 sumNormal = vec3(0.0);
    float sumWeight = 0.0;
    
    float radius = uBevelRadius;
    if (uBevelScaleWithDistance == 1) {
        int camIdx = 0;
        if (uSplitMode == 1 && uv.x >= 0.5) {
            camIdx = 1;
        }
        
        float nearVal = uNear[camIdx];
        float farVal = uFar[camIdx];
        float targetDist = uTargetDistance[camIdx];
        int projType = uProjType[camIdx];
        float orthoZoom = uOrthoZoom[camIdx];
        float fov = uFov[camIdx];
        float vpHeight = uViewportHeight[camIdx];
        
        if (projType == 0) { // Perspective
            float zDepth = (farVal * nearVal) / (farVal - D0 * (farVal - nearVal));
            float scale = targetDist / max(zDepth, 0.001);
            radius = uBevelRadius * scale;
        } else { // Orthographic
            float tanHalfFov = tan(fov * 0.5 * 3.14159265 / 180.0);
            float refOrthoZoom = targetDist * tanHalfFov / vpHeight;
            float scale = refOrthoZoom / max(orthoZoom, 0.00001);
            radius = uBevelRadius * scale;
        }
    }
    
    int r = int(round(radius));
    if (r < 1) r = 1;
    if (r > 8) r = 8; // Performance safeguard
    
    float sigma = radius;
    float twoSigmaSq = 2.0 * sigma * sigma;
    
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            vec2 offset = vec2(float(x), float(y)) * uInvViewportSize;
            vec2 neighborUv = uv + offset;
            
            float Di = texture(uDepthMap, neighborUv).r;
            if (Di >= 1.0) continue;
            
            vec3 Ni = texture(uNormalMap, neighborUv).rgb * 2.0 - 1.0;
            
            float depthDiff = abs(Di - D0);
            // Allow larger tolerance near edges but reject true silhouettes
            float depthThreshold = (abs(float(x) * dx) + abs(float(y) * dy)) * 2.0 + 0.0002;
            
            if (depthDiff < depthThreshold) {
                float dist = length(vec2(x, y));
                float spatialWeight = exp(-(dist * dist) / twoSigmaSq);
                
                sumNormal += Ni * spatialWeight;
                sumWeight += spatialWeight;
            }
        }
    }
    
    vec3 N_smooth = sumWeight > 0.0 ? normalize(sumNormal) : N0;
    vec3 N_final = normalize(mix(N0, N_smooth, clamp(uBevelStrength, 0.0, 1.0)));
    
    fragColor = vec4(N_final * 0.5 + 0.5, 1.0);
}
