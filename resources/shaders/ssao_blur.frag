#version 300 es
precision highp float;
out vec4 fragColor;
in vec2 vTexCoord;

uniform sampler2D uSsaoTex;
uniform sampler2D uDepthTex;
uniform vec2 uUvMax;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(uSsaoTex, 0));
    float result = 0.0;
    float weightTotal = 0.0;
    
    vec2 centerUv = clamp(vTexCoord, vec2(0.0), uUvMax);
    float centerDepth = texture(uDepthTex, centerUv).r;
    
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            vec2 sampleUv = clamp(vTexCoord + offset, vec2(0.0), uUvMax);
            float rDepth = texture(uDepthTex, sampleUv).r;
            
            // Bilateral weight based on depth similarity
            float weight = 1.0 / (abs(centerDepth - rDepth) * 1000.0 + 1.0);
            
            result += texture(uSsaoTex, sampleUv).r * weight;
            weightTotal += weight;
        }
    }
    
    float ao = result / weightTotal;
    fragColor = vec4(ao, ao, ao, 1.0);
}
