#version 300 es
precision highp float;

out vec4 fragColor;

in vec2 vTexCoord;

uniform sampler2D uDepthTex;       // Depth buffer texture
uniform sampler2D uNormalsTex;     // View-space normals map
uniform sampler2D uNoiseTex;       // 4x4 rotation noise

uniform vec3 uSamples[64];
uniform mat4 uProjection[2];
uniform mat4 uInvProjection[2];
uniform int uSplitMode;

uniform vec2 uNoiseScale;          // screen_resolution / 4.0
uniform float uRadius;             // AO effect radius
uniform float uBias;               // Depth bias to prevent self-shadowing

// Reconstruct view-space position from depth
vec3 getViewPos(vec2 uv, int camIdx) {
    float depth = texture(uDepthTex, uv).r;
    // Map NDC back to view-space
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 viewPos = uInvProjection[camIdx] * clipPos;
    return viewPos.xyz / viewPos.w;
}

void main() {
    int camIdx = (vTexCoord.x < 0.5 && uSplitMode == 1) ? 0 : 1;
    
    // In split mode, the texture coordinate needs to be scaled to [0, 1] range for the noise texture wrap,
    // but uNoiseScale already takes care of the screen space frequency, so vTexCoord * uNoiseScale is fine.
    
    vec3 fragPos = getViewPos(vTexCoord, camIdx);
    
    // View-space normals are stored mapped to [0,1], unpack to [-1,1]
    vec3 normal = texture(uNormalsTex, vTexCoord).rgb * 2.0 - 1.0;
    normal = normalize(normal);
    
    vec3 randomVec = texture(uNoiseTex, vTexCoord * uNoiseScale).rgb;
    
    // Gram-Schmidt process to create tangent-to-view space matrix (TBN)
    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);
    
    float occlusion = 0.0;
    for(int i = 0; i < 64; ++i) {
        // From tangent to view space
        vec3 samplePos = TBN * uSamples[i];
        samplePos = fragPos + samplePos * uRadius;
        
        // Project sample position to screen/texture space
        vec4 offset = vec4(samplePos, 1.0);
        offset = uProjection[camIdx] * offset;
        offset.xyz /= offset.w;
        vec2 sampleUV = offset.xy * 0.5 + 0.5;
        
        // Get depth of sample's projected point on the actual geometry
        vec3 actualPos = getViewPos(sampleUV, camIdx);
        
        // Range check to prevent depth-bleeding on far edges
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / abs(fragPos.z - actualPos.z));
        occlusion += (actualPos.z >= samplePos.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }
    
    // Invert occlusion factor so 1.0 = no occlusion, 0.0 = full occlusion
    float ao = 1.0 - (occlusion / 64.0);
    fragColor = vec4(ao, ao, ao, 1.0);
}
