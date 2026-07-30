#version 300 es
precision highp float;
uniform sampler2D uTexture0;
uniform vec3 uColor;
uniform float uSelected;
in vec3 vVertex;
in vec3 vVertexPres;
in vec3 vNormal;
out vec4 fragColor;
vec3 sRGBToLinear(vec3 color) { return pow(color, vec3(2.2)); }
vec4 encodeRGBM(const in vec3 col) {
  vec4 rgbm;
  vec3 color = col / 5.0;
  rgbm.a = clamp(max(max(color.r, color.g), max(color.b, 1e-6)), 0.0, 1.0);
  rgbm.a = ceil(rgbm.a * 255.0) / 255.0;
  rgbm.rgb = color / rgbm.a;
  return rgbm;
}

void main() {
  // Use smooth vertex normals, aligned to face forward using the flat normal
  vec3 N = normalize(vNormal);
  vec3 flatNormal = -normalize(cross(dFdy(vVertex), dFdx(vVertex)));
  vec3 normal = dot(N, flatNormal) < 0.0 ? -N : N;

  vec3 nm_z = normalize(vVertexPres);
  vec3 nm_x = vec3(-nm_z.z, 0.0, nm_z.x);
  vec3 nm_y = cross(nm_x, nm_z);
  vec2 texCoord = 0.5 + 0.5 * vec2(dot(normal, nm_x), dot(normal, nm_y));
  vec3 matcapColor = sRGBToLinear(texture(uTexture0, texCoord).rgb);
  vec3 baseColor = matcapColor * sRGBToLinear(uColor);
  
  // Ensure the armature remains visible even under dark matcaps
  baseColor = mix(baseColor, sRGBToLinear(uColor), 0.2);

  if (uSelected > 0.5) {
    baseColor = mix(baseColor, vec3(1.0, 1.0, 0.0), 0.3) + vec3(0.2, 0.2, 0.0);
  }
  fragColor = encodeRGBM(baseColor);
}
