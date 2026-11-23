#version 450

layout(set = 0, binding = 1) uniform sampler2D uGlyphAtlas;
layout(set = 0, binding = 2) uniform sampler2D uCompanionAtlas;

layout(push_constant) uniform PushConstants {
  vec4 viewport;  // xy = width, height; z = target_is_srgb; w = dpr
} pc;

layout(location = 0) in vec2 vLocalPos;
layout(location = 1) in vec2 vLocalUv;
layout(location = 2) in vec2 vSize;
layout(location = 3) in vec4 vClip;
layout(location = 4) in vec4 vRadii;
layout(location = 5) in vec4 vColor;
layout(location = 6) in vec4 vColor2;
layout(location = 7) in vec4 vUv;
layout(location = 8) in vec4 vParams;

layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 c) {
  vec3 low = c / 12.92;
  vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(low, high, step(vec3(0.04045), c));
}

// Signed distance to a rounded box centered at origin with half-extents b.
// r = (tr, br, tl, bl) -> we remap from vRadii = (tl, tr, br, bl).
float sdRoundedBox(vec2 p, vec2 b, vec4 radii) {
  float r_right = (p.y > 0.0) ? radii.z : radii.y;
  float r_left  = (p.y > 0.0) ? radii.w : radii.x;
  float r = (p.x > 0.0) ? r_right : r_left;
  r = min(r, min(b.x, b.y));
  vec2 q = abs(p) - b + vec2(r);
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float sdSegment(vec2 p, vec2 a, vec2 b) {
  vec2 pa = p - a;
  vec2 ba = b - a;
  float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
  return length(pa - ba * h);
}

float sdTriangle(vec2 p, vec2 p0, vec2 p1, vec2 p2) {
  vec2 e0 = p1 - p0;
  vec2 e1 = p2 - p1;
  vec2 e2 = p0 - p2;
  vec2 v0 = p - p0;
  vec2 v1 = p - p1;
  vec2 v2 = p - p2;
  vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / max(dot(e0, e0), 1e-6), 0.0, 1.0);
  vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / max(dot(e1, e1), 1e-6), 0.0, 1.0);
  vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / max(dot(e2, e2), 1e-6), 0.0, 1.0);
  float s = sign(e0.x * e2.y - e0.y * e2.x);
  vec2 d = min(min(
    vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
    vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
    vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
  return -sqrt(d.x) * sign(d.y);
}

float sdArc(vec2 p, float radius, float startAngle, float sweepAngle, float halfThick) {
  float ang = atan(p.y, p.x);
  float rel = ang - startAngle;
  // Wrap to [-pi, pi] around half sweep
  const float TWO_PI = 6.28318530718;
  float halfSweep = sweepAngle * 0.5;
  rel -= halfSweep;
  rel = mod(rel + 3.14159265359, TWO_PI) - 3.14159265359;
  if (abs(rel) <= halfSweep) {
    return abs(length(p) - radius) - halfThick;
  }
  vec2 e0 = vec2(cos(startAngle), sin(startAngle)) * radius;
  vec2 e1 = vec2(cos(startAngle + sweepAngle), sin(startAngle + sweepAngle)) * radius;
  return min(length(p - e0), length(p - e1)) - halfThick;
}

void main() {
  // Per-instance scissor clip in physical pixel space.
  if (gl_FragCoord.x < vClip.x || gl_FragCoord.y < vClip.y ||
      gl_FragCoord.x > vClip.x + vClip.z || gl_FragCoord.y > vClip.y + vClip.w) {
    discard;
  }

  int kind = int(vParams.x + 0.5);
  vec4 col = vec4(0.0);

  if (kind == 0 || kind == 3) {
    // 0: Solid rounded box (+ optional crisp inner border)
    // 3: Vertical gradient rounded box (vColor -> vColor2)
    vec2 halfSize = vSize * 0.5;
    vec2 p = vLocalPos - halfSize;
    float d = sdRoundedBox(p, halfSize, vRadii);
    float outerAlpha = 1.0 - smoothstep(-0.6, 0.6, d);
    if (outerAlpha <= 0.001) {
      discard;
    }

    vec4 fillCol = (kind == 3) ? mix(vColor, vColor2, clamp(vLocalUv.y, 0.0, 1.0)) : vColor;
    float borderW = vParams.y;
    if (kind == 0 && borderW > 0.001 && vColor2.a > 0.001) {
      float innerD = d + borderW;
      float borderFactor = smoothstep(-0.6, 0.6, innerD);
      // Premultiply-safe blend of border over fill
      vec4 b = vColor2;
      vec3 rgb = mix(fillCol.rgb, b.rgb, borderFactor * b.a);
      float a = mix(fillCol.a, max(fillCol.a, b.a), borderFactor);
      col = vec4(rgb, a * outerAlpha);
    } else {
      col = vec4(fillCol.rgb, fillCol.a * outerAlpha);
    }
  } else if (kind == 1) {
    // FreeType2 R8 glyph atlas
    vec2 uv = mix(vUv.xy, vUv.zw, vLocalUv);
    float cov = texture(uGlyphAtlas, uv).r;
    if (cov <= 0.002) {
      discard;
    }
    // Gentle gamma contrast boost for crisp CJK/Latin readability
    float gamma = (vParams.z > 0.01) ? vParams.z : 0.88;
    cov = pow(cov, gamma);
    col = vec4(vColor.rgb, vColor.a * cov);
  } else if (kind == 2) {
    // Analytical soft drop shadow for rounded box
    float blur = max(vParams.z, 1.0);
    vec2 halfSize = max(vSize * 0.5 - vec2(blur), vec2(1.0));
    vec2 p = vLocalPos - vSize * 0.5;
    float d = sdRoundedBox(p, halfSize, vRadii);
    float t = clamp(1.0 - smoothstep(-blur * 0.25, blur, d), 0.0, 1.0);
    float alpha = t * t * vColor.a;
    if (alpha <= 0.001) {
      discard;
    }
    col = vec4(vColor.rgb, alpha);
  } else if (kind == 4) {
    // Antialiased filled triangle (local normalized coords in vRadii.xy, vRadii.zw, vColor2.xy)
    vec2 p0 = vRadii.xy * vSize;
    vec2 p1 = vRadii.zw * vSize;
    vec2 p2 = vColor2.xy * vSize;
    float roundR = vParams.z;
    float d = sdTriangle(vLocalPos, p0, p1, p2) - roundR;
    float a = 1.0 - smoothstep(-0.6, 0.6, d);
    if (a <= 0.001) {
      discard;
    }
    col = vec4(vColor.rgb, vColor.a * a);
  } else if (kind == 5) {
    // Antialiased capsule / line segment from p0=vRadii.xy to p1=vRadii.zw (normalized)
    vec2 p0 = vRadii.xy * vSize;
    vec2 p1 = vRadii.zw * vSize;
    float halfW = max(vParams.y * 0.5, 0.5);
    float d = sdSegment(vLocalPos, p0, p1) - halfW;
    float a = 1.0 - smoothstep(-0.6, 0.6, d);
    if (a <= 0.001) {
      discard;
    }
    col = vec4(vColor.rgb, vColor.a * a);
  } else if (kind == 6) {
    // Antialiased circular arc (smiles, happy eyes, spinners, rings)
    vec2 p = vLocalPos - vSize * 0.5;
    float radius = vParams.z;
    float halfThick = max(vParams.y * 0.5, 0.5);
    float d = sdArc(p, radius, vUv.x, vUv.y, halfThick);
    float a = 1.0 - smoothstep(-0.6, 0.6, d);
    if (a <= 0.001) {
      discard;
    }
    col = vec4(vColor.rgb, vColor.a * a);
  } else if (kind == 7) {
    // Backgroundless 3D tactile figurine with real-time organic pivot deformation
    // (breathing squash-stretch, occasional head tilt, playful sway) + subtle studio rim light.
    // vRadii = (tilt_rad, squash_x, stretch_y, sway_x)
    // vParams = (7.0, hover_glow, rim_boost, opacity_mul)
    vec2 pivot = vec2(0.5, 0.88);
    vec2 p = vLocalUv - pivot;

    // Gentle upper-body sway anchored at the base
    float heightFactor = pow(clamp((pivot.y - vLocalUv.y) / max(pivot.y, 0.01), 0.0, 1.0), 1.35);
    p.x -= vRadii.w * heightFactor;

    // Breathing & bounce squash-and-stretch around feet
    p.x /= max(vRadii.y, 0.5);
    p.y /= max(vRadii.z, 0.5);

    // Organic tilt weighted smoothly toward the head/upper body
    float ang = -vRadii.x * (0.35 + 0.65 * heightFactor);
    float cs = cos(ang);
    float sn = sin(ang);
    p = vec2(p.x * cs - p.y * sn, p.x * sn + p.y * cs);

    vec2 warpedUv = p + pivot;
    if (warpedUv.x < 0.002 || warpedUv.x > 0.998 || warpedUv.y < 0.002 || warpedUv.y > 0.998) {
      discard;
    }

    vec2 atlasUv = mix(vUv.xy, vUv.zw, warpedUv);
    vec4 tex = texture(uCompanionAtlas, atlasUv);
    if (tex.a <= 0.015) {
      discard;
    }

    // Estimate 3D silhouette edge gradient for subtle top-left warm studio rim lighting
    vec2 stepUv = (vUv.zw - vUv.xy) * 0.006;
    float aL = texture(uCompanionAtlas, atlasUv - vec2(stepUv.x, 0.0)).a;
    float aR = texture(uCompanionAtlas, atlasUv + vec2(stepUv.x, 0.0)).a;
    float aT = texture(uCompanionAtlas, atlasUv - vec2(0.0, stepUv.y)).a;
    float aB = texture(uCompanionAtlas, atlasUv + vec2(0.0, stepUv.y)).a;
    vec2 grad = vec2(aR - aL, aB - aT);
    float rim = clamp(dot(grad, normalize(vec2(0.72, 0.68))), 0.0, 1.0);

    vec3 rgb = tex.rgb;
    // Soft warm studio rim along silhouette + subtle interactive hover warmth
    float rimStrength = 0.14 + 0.14 * vParams.z + 0.10 * vParams.y;
    rgb += vec3(1.0, 0.94, 0.85) * (rim * rimStrength);
    rgb += vec3(1.0, 0.97, 0.92) * (0.045 * vParams.y);
    col = vec4(clamp(rgb, 0.0, 1.0), tex.a);
  } else {
    discard;
  }

  col.a *= vParams.w;
  if (col.a <= 0.001) {
    discard;
  }

  if (pc.viewport.z > 0.5) {
    col.rgb = srgbToLinear(col.rgb);
  }
  outColor = col;
}
