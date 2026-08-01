#version 300 es
precision highp float;

in vec4 vColor;
in vec3 vWorldPos;

uniform vec3 uPivot;
uniform float uFogNear;
uniform float uFogFar;

out vec4 fragColor;

void main() {
    float dist = length(vWorldPos.xz - uPivot.xz);
    float fogFactor = clamp((uFogFar - dist) / max(uFogFar - uFogNear, 0.001), 0.0, 1.0);
    fogFactor = fogFactor * fogFactor * (3.0 - 2.0 * fogFactor);
    
    float alpha = vColor.a * fogFactor;
    if (alpha <= 0.001) {
        discard;
    }
    
    fragColor = vec4(vColor.rgb, alpha);
}
