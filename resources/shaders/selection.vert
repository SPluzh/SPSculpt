#version 300 es
layout(location = 0) in vec3 aVertex;
uniform mat4 uMVP;
uniform vec2 uViewportSize;
uniform float uOffsetPixels;
uniform bool uRef2DMode;
uniform vec2 uView2DOffset;
uniform float uView2DZoom;
void main() {
    vec4 clipPos = uMVP * vec4(aVertex, 1.0);
    if (uOffsetPixels != 0.0 && clipPos.w > 0.0) {
        float len = length(aVertex.xy);
        if (len > 0.0001) {
            vec2 dir = aVertex.xy / len;
            vec2 ndcOffset = (dir * uOffsetPixels * 2.0) / uViewportSize;
            clipPos.xy += ndcOffset * clipPos.w;
        }
    }
    if (uRef2DMode) {
        clipPos.xy = clipPos.xy * uView2DZoom + uView2DOffset * clipPos.w;
    }
    gl_Position = clipPos;
}
