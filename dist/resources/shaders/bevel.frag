#version 300 es
precision highp float;

in vec2 vTexCoord;

uniform sampler2D uNormalMap;
uniform sampler2D uDepthMap;

uniform vec2 uInvViewportSize;
uniform float uBevelRadius;
uniform float uBevelStrength;

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
    
    int r = int(round(uBevelRadius));
    if (r < 1) r = 1;
    if (r > 8) r = 8; // Performance safeguard
    
    float sigma = uBevelRadius;
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
