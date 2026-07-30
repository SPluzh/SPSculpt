#version 300 es
precision highp float;
out vec4 fragColor;
in vec2 vTexCoord;

uniform sampler2D uSsaoTex;
uniform sampler2D uDepthTex;

void main() {
    vec2 texelSize = 1.0 / vec2(textureSize(uSsaoTex, 0));
    float result = 0.0;
    float weightTotal = 0.0;
    
    float centerDepth = texture(uDepthTex, vTexCoord).r;
    
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float rDepth = texture(uDepthTex, vTexCoord + offset).r;
            
            // Bilateral weight based on depth similarity
            float weight = 1.0 / (abs(centerDepth - rDepth) * 1000.0 + 1.0);
            
            result += texture(uSsaoTex, vTexCoord + offset).r * weight;
            weightTotal += weight;
        }
    }
    
    float ao = result / weightTotal;
    fragColor = vec4(ao, ao, ao, 1.0);
}
