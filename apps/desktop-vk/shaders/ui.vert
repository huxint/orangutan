#version 450

struct UiInstance {
  vec4 rect;    // x, y, w, h (physical px)
  vec4 clip;    // x, y, w, h (physical px)
  vec4 radii;   // tl, tr, br, bl  OR  p0.xy, p1.xy
  vec4 color;   // primary RGBA
  vec4 color2;  // border RGBA / gradient bottom RGBA / p2.xy + extra
  vec4 uv;      // u0, v0, u1, v1  OR  arc start, sweep, inner, outer
  vec4 params;  // x=kind, y=border_or_thickness, z=blur_or_radius, w=opacity_mul
};

layout(std430, set = 0, binding = 0) readonly buffer InstanceBuffer {
  UiInstance instances[];
};

layout(push_constant) uniform PushConstants {
  vec4 viewport;  // xy = width, height; z = target_is_srgb (0 or 1); w = dpr
} pc;

layout(location = 0) out vec2 vLocalPos;
layout(location = 1) out vec2 vLocalUv;
layout(location = 2) out vec2 vSize;
layout(location = 3) out vec4 vClip;
layout(location = 4) out vec4 vRadii;
layout(location = 5) out vec4 vColor;
layout(location = 6) out vec4 vColor2;
layout(location = 7) out vec4 vUv;
layout(location = 8) out vec4 vParams;

const vec2 kCorners[6] = vec2[6](
  vec2(0.0, 0.0),
  vec2(1.0, 0.0),
  vec2(1.0, 1.0),
  vec2(0.0, 0.0),
  vec2(1.0, 1.0),
  vec2(0.0, 1.0)
);

void main() {
  UiInstance inst = instances[gl_InstanceIndex];
  vec2 corner = kCorners[gl_VertexIndex];
  vec2 px = inst.rect.xy + corner * inst.rect.zw;
  vec2 ndc = (px / pc.viewport.xy) * 2.0 - 1.0;

  gl_Position = vec4(ndc, 0.0, 1.0);
  vLocalPos = corner * inst.rect.zw;
  vLocalUv = corner;
  vSize = inst.rect.zw;
  vClip = inst.clip;
  vRadii = inst.radii;
  vColor = inst.color;
  vColor2 = inst.color2;
  vUv = inst.uv;
  vParams = inst.params;
}
