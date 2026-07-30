#version 300 es
layout(location = 0) in vec3 aVertex;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMVP;
uniform mat4 uMV;
uniform mat3 uN;
uniform float uRadiusTop;
uniform float uRadiusBottom;
out vec3 vVertex;
out vec3 vVertexPres;
out vec3 vNormal;
void main() {
  vec3 pos = aVertex;
  float u = pos.z; // cylinder Z goes 0 to 1
  float radius = mix(uRadiusBottom, uRadiusTop, u);
  pos.x *= radius;
  pos.y *= radius;
  vec3 norm = aNormal;
  if (uRadiusTop != uRadiusBottom && abs(norm.z) < 0.9) {
    float slope = (uRadiusTop - uRadiusBottom); 
    norm = vec3(aNormal.x, aNormal.y, -slope);
    norm = normalize(norm);
  }
  vNormal = normalize(uN * norm);
  vec4 v = uMV * vec4(pos, 1.0);
  vVertex = vec3(v);
  vVertexPres = vVertex / max(1.0, abs(uMV[3][2]));
  gl_Position = uMVP * vec4(pos, 1.0);
}
