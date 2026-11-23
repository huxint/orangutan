#version 450

layout(set = 0, binding = 1) uniform sampler2D uGlyphAtlas;

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

// ============================================================================
// 2D UI Analytical SDF Primitives
// ============================================================================

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

// ============================================================================
// Pure-Code Real-Time 3D Raymarched PBR Tactile Figurines (kind == 7)
// Zero PNG textures, zero cutouts, true 3D geometry + articulation + shadows!
// ============================================================================

struct Hit3D {
  float d;
  vec3 albedo;
  float mat;   // 0=wool_felt, 1=matte_vinyl/skin, 2=glass_eye, 3=leaf, 4=brass, 5=wood/ceramic
};

mat2 rot2(float a) {
  float c = cos(a);
  float s = sin(a);
  return mat2(c, -s, s, c);
}

float hash31(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.yzx + 33.33);
  return fract((p.x + p.y) * p.z);
}

float noise3(vec3 p) {
  vec3 i = floor(p);
  vec3 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(
    mix(mix(hash31(i + vec3(0,0,0)), hash31(i + vec3(1,0,0)), f.x),
        mix(hash31(i + vec3(0,1,0)), hash31(i + vec3(1,1,0)), f.x), f.y),
    mix(mix(hash31(i + vec3(0,0,1)), hash31(i + vec3(1,0,1)), f.x),
        mix(hash31(i + vec3(0,1,1)), hash31(i + vec3(1,1,1)), f.x), f.y),
    f.z
  );
}

float sdSphere3(vec3 p, float r) {
  return length(p) - r;
}

float sdEllipsoid3(vec3 p, vec3 r) {
  float k0 = length(p / r);
  float k1 = length(p / (r * r));
  return k0 * (k0 - 1.0) / max(k1, 1e-5);
}

float sdCapsule3(vec3 p, vec3 a, vec3 b, float r) {
  vec3 pa = p - a, ba = b - a;
  float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-5), 0.0, 1.0);
  return length(pa - ba * h) - r;
}

float sdRoundCone3(vec3 p, vec3 a, vec3 b, float r1, float r2) {
  vec3 ba = b - a;
  float l2 = dot(ba, ba);
  float rr = r1 - r2;
  float a2 = l2 - rr * rr;
  float il2 = 1.0 / max(l2, 1e-5);
  vec3 pa = p - a;
  float y = dot(pa, ba);
  float z = y - l2;
  vec3 x2v = pa * l2 - ba * y;
  float x2 = dot(x2v, x2v);
  float y2 = y * y * l2;
  float z2 = z * z * l2;
  float k = sign(rr) * rr * rr * x2;
  if (sign(z) * a2 * z2 > k) return sqrt(x2 + z2) * il2 - r2;
  if (sign(y) * a2 * y2 < k) return sqrt(x2 + y2) * il2 - r1;
  return (sqrt(x2 * a2 * il2) + y * rr) * il2 - r1;
}

float sdTorus3(vec3 p, vec2 t) {
  vec2 q = vec2(length(p.xz) - t.x, p.y);
  return length(q) - t.y;
}

float sdRoundBox3(vec3 p, vec3 b, float r) {
  vec3 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

float smin(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}

float smax(float a, float b, float k) {
  return -smin(-a, -b, k);
}

Hit3D opUnion(Hit3D a, Hit3D b) {
  return (a.d < b.d) ? a : b;
}

Hit3D opSmoothUnion(Hit3D a, Hit3D b, float k) {
  float h = clamp(0.5 + 0.5 * (b.d - a.d) / k, 0.0, 1.0);
  float d = mix(b.d, a.d, h) - k * h * (1.0 - h);
  vec3 col = mix(b.albedo, a.albedo, h);
  float mat = (h > 0.5) ? a.mat : b.mat;
  return Hit3D(d, col, mat);
}

// Helper: add symmetric glossy 3D glass eyes + animated 3D blinking eyelids!
Hit3D addEyes3D(Hit3D res, vec3 qh, vec3 eyePos, float eyeR, float blink, vec3 lidCol, float lidMat) {
  vec3 pe = vec3(abs(qh.x), qh.y, qh.z) - eyePos;
  // Slightly recessed socket ring for lifelike eye depth
  float dEye = sdSphere3(pe, eyeR);
  res = opUnion(res, Hit3D(dEye, vec3(0.035, 0.025, 0.020), 2.0));

  // Real 3D eyelid closing over the eyeball during blinks
  if (blink > 0.02) {
    float lidCut = mix(eyeR * 1.15, -eyeR * 0.95, blink);
    float dLid = max(sdSphere3(pe, eyeR * 1.09), -(pe.y - lidCut));
    res = opUnion(res, Hit3D(dLid, lidCol, lidMat));
  }
  return res;
}

// Global per-invocation 3D pose uniforms extracted from instance attributes
struct PetPose3D {
  int id;
  float time;
  float yaw;
  float tilt;
  float hopY;
  float blink;
  float breath;
  float wag;
};

Hit3D mapPet3D(vec3 p, PetPose3D pose) {
  // Base body transform: vertical hop + subtle breathing stretch anchored at floor y = -0.76
  vec3 pb = p - vec3(0.0, pose.hopY, 0.0);
  float by = pb.y + 0.76;
  pb.y = -0.76 + by / (1.0 + pose.breath * 0.028);
  pb.xz /= (1.0 - pose.breath * 0.014);

  // Articulated 3D head coordinates pivoted smoothly around neck (0, -0.10, 0)
  vec3 ph = pb - vec3(0.0, -0.08, 0.0);
  ph.xy = rot2(pose.tilt) * ph.xy;
  ph.xz = rot2(pose.yaw) * ph.xz;
  ph += vec3(0.0, -0.08, 0.0);

  Hit3D res = Hit3D(10.0, vec3(0.5), 0.0);

  if (pose.id == 0) {
    // ================= 0: ORAN (Baby Terracotta Orangutan + Twin Sprout) =================
    vec3 fur = vec3(0.76, 0.29, 0.11);
    vec3 cream = vec3(0.96, 0.85, 0.72);
    vec3 blush = vec3(0.90, 0.48, 0.39);
    vec3 noseC = vec3(0.32, 0.16, 0.10);

    // Chubby sitting furry body + cozy arms + tucked front feet
    float dBody = sdEllipsoid3(pb - vec3(0.0, -0.44, -0.02), vec3(0.41, 0.33, 0.34));
    dBody = smax(dBody, -(pb.y + 0.76), 0.04);  // flat planted sitting base
    vec3 pa = vec3(abs(pb.x), pb.y, pb.z);
    float dArm = sdCapsule3(pa, vec3(0.33, -0.32, 0.04), vec3(0.39, -0.58, 0.15), 0.125);
    float dFoot = sdEllipsoid3(pa - vec3(0.19, -0.68, 0.25), vec3(0.125, 0.09, 0.145));
    dBody = smin(dBody, dArm, 0.075);
    res = Hit3D(dBody, fur, 0.0);
    float soleMask = smoothstep(0.07, 0.02, length(pa - vec3(0.19, -0.68, 0.38)));
    res = opSmoothUnion(res, Hit3D(dFoot, mix(fur * 0.92, cream, soleMask * 0.65), 0.0), 0.04);

    // Warm round fluffy head + ears
    float dHead = sdEllipsoid3(ph - vec3(0.0, 0.12, 0.0), vec3(0.49, 0.46, 0.44));
    vec3 pe = vec3(abs(ph.x), ph.y, ph.z);
    float dEar = sdEllipsoid3(pe - vec3(0.49, 0.11, -0.02), vec3(0.14, 0.15, 0.09));
    float dEarCarve = sdSphere3(pe - vec3(0.50, 0.11, 0.07), 0.095);
    res = opSmoothUnion(res, Hit3D(dHead, fur, 0.0), 0.10);
    Hit3D earHit = Hit3D(smax(dEar, -dEarCarve, 0.03), cream, 1.0);
    float earShell = sdEllipsoid3(pe - vec3(0.47, 0.11, -0.05), vec3(0.145, 0.155, 0.085));
    res = opSmoothUnion(res, Hit3D(earShell, fur, 0.0), 0.04);
    res = opUnion(res, earHit);

    // Velvety cream face mask & protruding cute muzzle
    float dMask = sdEllipsoid3(ph - vec3(0.0, 0.13, 0.24), vec3(0.35, 0.34, 0.25));
    float dMuzzle = sdEllipsoid3(ph - vec3(0.0, -0.02, 0.38), vec3(0.24, 0.17, 0.19));
    float dFace = smin(dMask, dMuzzle, 0.08);
    // Airbrushed rosy cheeks on the cream face
    float cheekAmt = smoothstep(0.19, 0.04, length(vec3(abs(ph.x) - 0.22, ph.y - 0.02, ph.z - 0.38)));
    vec3 faceCol = mix(cream, blush, cheekAmt * 0.48);
    // Gentle curved sculpted smile groove
    vec3 pm = ph - vec3(0.0, -0.068, 0.545);
    float smileD = abs(pm.y + 0.038 * cos(clamp(pm.x * 8.5, -1.57, 1.57))) + max(abs(pm.x) - 0.135, 0.0) * 1.4;
    float smileGroove = smoothstep(0.026, 0.005, length(vec2(smileD, pm.z)));
    dFace += smileGroove * 0.015;
    faceCol = mix(faceCol, vec3(0.44, 0.22, 0.14), smileGroove * 0.78);
    res = opSmoothUnion(res, Hit3D(dFace, faceCol, 1.0), 0.025);

    // Cute sculpted dark nose
    float dNose = sdEllipsoid3(ph - vec3(0.0, 0.052, 0.548), vec3(0.068, 0.042, 0.042));
    res = opUnion(res, Hit3D(dNose, noseC, 5.0));

    // Glossy 3D glass eyes + blinking eyelids
    res = addEyes3D(res, ph, vec3(0.155, 0.155, 0.425), 0.080, pose.blink, cream, 1.0);

    // Living twin green sprout leaf swaying on head!
    vec3 ps = ph - vec3(0.0, 0.56, 0.02);
    ps.xy = rot2(sin(pose.time * 2.8) * 0.09 + pose.tilt * 0.4) * ps.xy;
    float dStem = sdCapsule3(ps, vec3(0.0, -0.04, 0.0), vec3(0.0, 0.17, 0.0), 0.027);
    vec3 pl1 = ps - vec3(-0.145, 0.235, 0.0);
    pl1.xy = rot2(-0.52) * pl1.xy;
    float dLeaf1 = sdEllipsoid3(pl1, vec3(0.175, 0.078, 0.036));
    vec3 pl2 = ps - vec3(0.155, 0.255, 0.01);
    pl2.xy = rot2(0.46) * pl2.xy;
    float dLeaf2 = sdEllipsoid3(pl2, vec3(0.165, 0.074, 0.036));
    float dSprout = smin(dStem, min(dLeaf1, dLeaf2), 0.03);
    vec3 leafCol = mix(vec3(0.24, 0.54, 0.19), vec3(0.50, 0.78, 0.27), clamp((ps.y - 0.10) * 3.5, 0.0, 1.0));
    res = opUnion(res, Hit3D(dSprout, leafCol, 3.0));
  } else if (pose.id == 1) {
    // ================= 1: SCOUT (Sitting Golden Forest Fox + Leather Collar) =================
    vec3 fur = vec3(0.84, 0.41, 0.13);
    vec3 cream = vec3(0.97, 0.92, 0.84);
    vec3 dark = vec3(0.19, 0.11, 0.07);
    vec3 leather = vec3(0.42, 0.18, 0.07);
    vec3 brass = vec3(0.88, 0.68, 0.24);

    // Sitting body + cream chest bib
    float dBody = sdEllipsoid3(pb - vec3(0.0, -0.45, -0.03), vec3(0.33, 0.32, 0.30));
    dBody = smax(dBody, -(pb.y + 0.76), 0.03);
    float chestMask = smoothstep(0.16, 0.02, length(vec2(pb.x * 1.1, (pb.y + 0.40) * 0.7))) * step(0.06, pb.z);
    res = Hit3D(dBody, mix(fur, cream, chestMask), 0.0);

    // Cute espresso front paws
    vec3 pp = vec3(abs(pb.x), pb.y, pb.z);
    float dPaw = sdCapsule3(pp, vec3(0.12, -0.48, 0.15), vec3(0.11, -0.70, 0.22), 0.088);
    res = opSmoothUnion(res, Hit3D(dPaw, mix(fur, dark, smoothstep(-0.50, -0.60, pb.y)), 0.0), 0.04);

    // Fluffy curled fox tail wagging gently in 3D!
    vec3 pt = pb - vec3(0.34, -0.56, -0.02);
    pt.xz = rot2(sin(pose.time * 3.2 + pose.wag) * 0.13) * pt.xz;
    float dTail = sdCapsule3(pt, vec3(-0.08, 0.02, -0.14), vec3(0.20, 0.05, 0.16), 0.175);
    float tipBlend = smoothstep(0.02, 0.19, pt.z);
    res = opSmoothUnion(res, Hit3D(dTail, mix(fur, cream, tipBlend), 0.0), 0.06);

    // Cognac leather collar + miniature gleaming brass buckle
    vec3 pc = pb - vec3(0.0, -0.17, 0.02);
    float dCollar = sdTorus3(pc, vec2(0.24, 0.048));
    res = opUnion(res, Hit3D(dCollar, leather, 5.0));
    float dBuckle = sdRoundBox3(pc - vec3(0.0, 0.0, 0.255), vec3(0.062, 0.048, 0.026), 0.012);
    res = opUnion(res, Hit3D(dBuckle, brass, 4.0));

    // Fox head + fluffy cheek tufts
    float dHead = sdEllipsoid3(ph - vec3(0.0, 0.14, 0.02), vec3(0.44, 0.37, 0.38));
    vec3 pfh = vec3(abs(ph.x), ph.y, ph.z);
    float dCheek = sdEllipsoid3(pfh - vec3(0.34, 0.04, 0.10), vec3(0.20, 0.15, 0.18));
    float dSnout = sdEllipsoid3(ph - vec3(0.0, 0.03, 0.34), vec3(0.16, 0.12, 0.17));
    float dHeadAll = smin(smin(dHead, dCheek, 0.08), dSnout, 0.07);
    float whiteCheek = smoothstep(0.13, -0.03, ph.y - 0.04 * abs(ph.x));
    float rosy = smoothstep(0.16, 0.03, length(pfh - vec3(0.24, 0.06, 0.30)));
    vec3 headCol = mix(fur, cream, whiteCheek * smoothstep(-0.08, 0.15, ph.z));
    headCol = mix(headCol, vec3(0.92, 0.50, 0.42), rosy * 0.42);
    res = opSmoothUnion(res, Hit3D(dHeadAll, headCol, 0.0), 0.05);

    // Pointy dark-tipped fox ears with cream inside
    float dEar = sdRoundCone3(pfh, vec3(0.26, 0.36, 0.0), vec3(0.36, 0.72, -0.02), 0.15, 0.035);
    float earTip = smoothstep(0.55, 0.67, pfh.y);
    float earInner = smoothstep(0.11, 0.03, length(pfh - vec3(0.29, 0.52, 0.10)));
    vec3 earCol = mix(mix(fur, dark, earTip), cream, earInner * (1.0 - earTip));
    res = opSmoothUnion(res, Hit3D(dEar, earCol, 0.0), 0.04);

    // Nose + glossy glass eyes
    res = opUnion(res, Hit3D(sdEllipsoid3(ph - vec3(0.0, 0.07, 0.50), vec3(0.060, 0.044, 0.044)), dark, 5.0));
    res = addEyes3D(res, ph, vec3(0.165, 0.16, 0.365), 0.074, pose.blink, fur, 0.0);
  } else if (pose.id == 2) {
    // ================= 2: MOSS (Plump Sage Owl + Antique Brass Spectacles) =================
    vec3 sage = vec3(0.32, 0.48, 0.34);
    vec3 lightSage = vec3(0.75, 0.82, 0.73);
    vec3 belly = vec3(0.92, 0.89, 0.79);
    vec3 brass = vec3(0.78, 0.56, 0.20);
    vec3 wood = vec3(0.76, 0.40, 0.12);

    // Plump oval wool owl body (head + torso integrated, subtly pivoting upper half)
    vec3 po = mix(pb, ph, smoothstep(-0.35, 0.25, pb.y));
    float dBody = sdEllipsoid3(po - vec3(0.0, -0.10, 0.0), vec3(0.52, 0.63, 0.48));
    dBody = smax(dBody, -(pb.y + 0.74), 0.05);

    // Cute folded wings & ear tufts
    vec3 sym = vec3(abs(po.x), po.y, po.z);
    float dWing = sdEllipsoid3(sym - vec3(0.46, -0.22, -0.02), vec3(0.13, 0.34, 0.24));
    float dTuft = sdRoundCone3(sym, vec3(0.28, 0.42, 0.02), vec3(0.49, 0.57, -0.02), 0.12, 0.04);
    float dOwl = smin(smin(dBody, dWing, 0.06), dTuft, 0.06);

    // Spotted cream belly + twin facial discs
    float bellyMask = smoothstep(0.06, -0.16, po.y) * smoothstep(0.42, 0.24, abs(po.x)) * smoothstep(0.05, 0.26, po.z);
    float spots = smoothstep(0.60, 0.73, noise3(po * 14.0 + vec3(2.3, 1.1, 0.0)));
    vec3 bellyCol = mix(belly, sage * 0.90, spots * 0.52);
    vec3 owlCol = mix(sage, bellyCol, bellyMask);
    res = Hit3D(dOwl, owlCol, 0.0);

    // Little carved wooden owl feet peeking at base
    vec3 pf = vec3(abs(pb.x), pb.y, pb.z);
    float dFoot = sdEllipsoid3(pf - vec3(0.16, -0.71, 0.14), vec3(0.088, 0.052, 0.105));
    res = opUnion(res, Hit3D(dFoot, vec3(0.56, 0.36, 0.18), 5.0));

    // Soft raised pale-sage facial feather discs
    float dDisc = sdEllipsoid3(sym - vec3(0.19, 0.16, 0.34), vec3(0.22, 0.22, 0.14));
    res = opSmoothUnion(res, Hit3D(dDisc, lightSage, 0.0), 0.04);

    // Crisp antique brass round spectacles (true 3D tori + nose bridge!)
    vec3 pg = vec3(abs(po.x), po.y, po.z) - vec3(0.195, 0.16, 0.495);
    float dFrame = sdTorus3(pg.xzy, vec2(0.168, 0.026));
    float dBridge = sdCapsule3(po, vec3(-0.042, 0.195, 0.50), vec3(0.042, 0.195, 0.50), 0.021);
    res = opUnion(res, Hit3D(min(dFrame, dBridge), brass, 4.0));

    // Carved warm wooden beak
    float dBeak = sdRoundCone3(po, vec3(0.0, 0.11, 0.48), vec3(0.0, -0.04, 0.565), 0.072, 0.019);
    res = opUnion(res, Hit3D(dBeak, wood, 5.0));

    // Big glossy owl glass eyes behind the brass spectacles!
    res = addEyes3D(res, po, vec3(0.195, 0.16, 0.415), 0.096, pose.blink, lightSage, 0.0);
  } else if (pose.id == 3) {
    // ================= 3: FERN (Slate-Blue Cat + Knitted Green Wool Scarf) =================
    vec3 slate = vec3(0.32, 0.45, 0.56);
    vec3 cream = vec3(0.95, 0.90, 0.81);
    vec3 scarfC = vec3(0.30, 0.48, 0.28);
    vec3 coral = vec3(0.90, 0.44, 0.38);
    vec3 peach = vec3(0.91, 0.68, 0.56);

    // Plump sitting cat body + cream front paws
    float dBody = sdEllipsoid3(pb - vec3(0.0, -0.45, -0.02), vec3(0.37, 0.32, 0.33));
    dBody = smax(dBody, -(pb.y + 0.76), 0.03);
    res = Hit3D(dBody, slate, 0.0);
    vec3 pp = vec3(abs(pb.x), pb.y, pb.z);
    float dPaw = sdEllipsoid3(pp - vec3(0.14, -0.68, 0.22), vec3(0.105, 0.085, 0.12));
    res = opSmoothUnion(res, Hit3D(dPaw, cream, 0.0), 0.03);

    // Curled cat tail swishing softly on left!
    vec3 pt = pb - vec3(-0.36, -0.54, -0.02);
    pt.xy = rot2(sin(pose.time * 2.4 + 1.0) * 0.11) * pt.xy;
    float dTail = sdCapsule3(pt, vec3(0.06, -0.10, -0.06), vec3(-0.14, 0.06, 0.08), 0.105);
    res = opSmoothUnion(res, Hit3D(dTail, slate, 0.0), 0.05);

    // Knitted moss-green wool scarf + side bow knot!
    vec3 ps = pb - vec3(0.0, -0.16, 0.02);
    float knitRib = sin(atan(ps.z, ps.x) * 26.0) * 0.007;
    float dScarf = sdTorus3(ps, vec2(0.30, 0.075)) + knitRib;
    float dBowL = sdEllipsoid3(ps - vec3(0.24, -0.03, 0.24), vec3(0.088, 0.066, 0.055));
    float dBowR = sdEllipsoid3(ps - vec3(0.35, -0.05, 0.19), vec3(0.082, 0.062, 0.055));
    float dRibbon = sdCapsule3(ps, vec3(0.28, -0.06, 0.23), vec3(0.25, -0.25, 0.27), 0.044);
    res = opUnion(res, Hit3D(smin(min(dScarf, dRibbon), min(dBowL, dBowR), 0.03), scarfC, 0.0));

    // Rounded cat head + perky pointed ears with peach insides
    float dHead = sdEllipsoid3(ph - vec3(0.0, 0.16, 0.02), vec3(0.46, 0.39, 0.40));
    vec3 pfh = vec3(abs(ph.x), ph.y, ph.z);
    float dEar = sdRoundCone3(pfh, vec3(0.25, 0.38, 0.01), vec3(0.34, 0.69, 0.0), 0.14, 0.045);
    float innerEar = smoothstep(0.115, 0.035, length(pfh - vec3(0.28, 0.51, 0.105)));
    res = opSmoothUnion(res, Hit3D(smin(dHead, dEar, 0.05), mix(slate, peach, innerEar), 0.0), 0.05);

    // Plump twin cream whisker pads + coral pink nose
    float dPad = sdEllipsoid3(pfh - vec3(0.10, 0.03, 0.38), vec3(0.135, 0.105, 0.115));
    res = opSmoothUnion(res, Hit3D(dPad, cream, 0.0), 0.03);
    res = opUnion(res, Hit3D(sdEllipsoid3(ph - vec3(0.0, 0.082, 0.478), vec3(0.050, 0.035, 0.036)), coral, 5.0));

    // Glossy cat eyes
    res = addEyes3D(res, ph, vec3(0.175, 0.18, 0.375), 0.077, pose.blink, slate, 0.0);
  } else if (pose.id == 4) {
    // ================= 4: PEBBLE (Sturdy Chestnut Hedgehog + Oak Shield & Brass Rivet) =================
    vec3 quillC = vec3(0.34, 0.19, 0.10);
    vec3 woodBody = vec3(0.48, 0.29, 0.15);
    vec3 shieldC = vec3(0.72, 0.46, 0.21);
    vec3 cream = vec3(0.95, 0.87, 0.74);
    vec3 blush = vec3(0.89, 0.52, 0.44);
    vec3 brass = vec3(0.90, 0.72, 0.26);

    // Carved smooth oak lower body + tucked paws holding shield
    float dBody = sdEllipsoid3(pb - vec3(0.0, -0.36, -0.01), vec3(0.45, 0.40, 0.40));
    dBody = smax(dBody, -(pb.y + 0.75), 0.04);
    vec3 pp = vec3(abs(pb.x), pb.y, pb.z);
    float dPaw = sdEllipsoid3(pp - vec3(0.25, -0.44, 0.32), vec3(0.11, 0.078, 0.098));
    res = Hit3D(smin(dBody, dPaw, 0.04), woodBody, 5.0);

    // Sculpted heraldic oak shield + gleaming brass boss rivet
    vec3 ps = pb - vec3(0.0, -0.44, 0.415);
    float shieldTaper = 1.0 - clamp((-ps.y) * 1.8, 0.0, 0.72);
    float dShield = sdRoundBox3(vec3(ps.x / max(shieldTaper, 0.28), ps.y, ps.z), vec3(0.145, 0.165, 0.028), 0.024);
    float rim = smoothstep(0.09, 0.15, max(abs(ps.x) / max(shieldTaper, 0.3), abs(ps.y)));
    res = opUnion(res, Hit3D(dShield, mix(shieldC, woodBody * 0.92, rim * 0.35), 5.0));
    res = opUnion(res, Hit3D(sdSphere3(ps - vec3(0.0, 0.01, 0.052), 0.036), brass, 4.0));

    // Cozy layered chestnut wool quill crown around head (true 3D bumpy dome!)
    vec3 pq = ph - vec3(0.0, 0.14, -0.08);
    float ang = atan(pq.y, pq.x);
    float bump = sin(ang * 13.0) * 0.034 + sin(pq.z * 18.0 + ang * 7.0) * 0.024;
    float dQuills = sdEllipsoid3(pq, vec3(0.57, 0.54, 0.48)) + bump;
    res = opSmoothUnion(res, Hit3D(dQuills, quillC, 0.0), 0.05);

    // Super soft cream-oatmeal wool face + tiny round ears + rosy cheeks
    float dFace = sdEllipsoid3(ph - vec3(0.0, 0.10, 0.18), vec3(0.41, 0.34, 0.32));
    vec3 pfh = vec3(abs(ph.x), ph.y, ph.z);
    float dEar = sdEllipsoid3(pfh - vec3(0.36, 0.30, 0.12), vec3(0.095, 0.095, 0.065));
    float rosy = smoothstep(0.18, 0.04, length(pfh - vec3(0.24, 0.01, 0.40)));
    vec3 fCol = mix(cream, blush, rosy * 0.48);
    res = opSmoothUnion(res, Hit3D(smin(dFace, dEar, 0.03), fCol, 0.0), 0.03);

    // Dark shiny nose + earnest glossy glass eyes
    res = opUnion(res, Hit3D(sdEllipsoid3(ph - vec3(0.0, 0.05, 0.49), vec3(0.066, 0.048, 0.048)), vec3(0.20, 0.12, 0.08), 5.0));
    res = addEyes3D(res, ph, vec3(0.165, 0.145, 0.435), 0.076, pose.blink, cream, 0.0);
  } else {
    // ================= 5: PIP (Plump Sunny Golden Songbird + Ceramic Beak) =================
    vec3 gold = vec3(0.90, 0.61, 0.16);
    vec3 wingC = vec3(0.78, 0.47, 0.12);
    vec3 belly = vec3(0.98, 0.93, 0.81);
    vec3 beakC = vec3(0.90, 0.39, 0.18);
    vec3 blush = vec3(0.94, 0.50, 0.38);

    vec3 po = mix(pb, ph, smoothstep(-0.30, 0.30, pb.y));

    // Plump round golden songbird body with soft cream chest belly
    float dBody = sdEllipsoid3(po - vec3(0.0, -0.14, 0.0), vec3(0.49, 0.51, 0.46));
    float bellyM = smoothstep(0.16, -0.12, po.y) * smoothstep(0.42, 0.18, abs(po.x)) * smoothstep(0.05, 0.26, po.z);
    vec3 sym = vec3(abs(po.x), po.y, po.z);
    float rosy = smoothstep(0.16, 0.03, length(sym - vec3(0.27, 0.04, 0.36)));
    vec3 bCol = mix(gold, belly, bellyM);
    bCol = mix(bCol, blush, rosy * 0.48);
    res = Hit3D(dBody, bCol, 0.0);

    // Cute folded layered side wings (flutter slightly when hopping!)
    vec3 pw = sym - vec3(0.44, -0.16, -0.02);
    pw.xy = rot2(-0.18 - pose.hopY * 1.4) * pw.xy;
    float dWing = sdEllipsoid3(pw, vec3(0.14, 0.26, 0.26));
    res = opSmoothUnion(res, Hit3D(dWing, wingC, 0.0), 0.04);

    // Playful bouncy twin top feather crest!
    vec3 pc = po - vec3(0.0, 0.38, 0.02);
    pc.xy = rot2(sin(pose.time * 3.6) * 0.08) * pc.xy;
    float dCrestL = sdCapsule3(pc, vec3(-0.02, -0.02, 0.0), vec3(-0.10, 0.22, -0.02), 0.072);
    float dCrestR = sdCapsule3(pc, vec3(0.03, -0.02, 0.0), vec3(0.11, 0.20, 0.01), 0.068);
    res = opSmoothUnion(res, Hit3D(min(dCrestL, dCrestR), gold, 0.0), 0.04);

    // Cute little tail feathers behind
    float dTail = sdEllipsoid3(po - vec3(0.0, -0.34, -0.46), vec3(0.18, 0.11, 0.18));
    res = opSmoothUnion(res, Hit3D(dTail, belly, 0.0), 0.05);

    // Tiny perched bird feet at floor
    vec3 pf = vec3(abs(pb.x), pb.y, pb.z);
    float dFoot = sdEllipsoid3(pf - vec3(0.13, -0.69, 0.06), vec3(0.08, 0.055, 0.11));
    res = opUnion(res, Hit3D(dFoot, vec3(0.54, 0.36, 0.21), 5.0));

    // Smooth matte-ceramic tangerine beak
    float dBeak = sdRoundCone3(po, vec3(0.0, 0.04, 0.43), vec3(0.0, 0.015, 0.565), 0.082, 0.024);
    res = opUnion(res, Hit3D(dBeak, beakC, 5.0));

    // Glossy jet-black glass bead eyes (wide-set chirpy songbird expression!)
    res = addEyes3D(res, po, vec3(0.205, 0.13, 0.385), 0.075, pose.blink, gold, 0.0);
  }

  return res;
}

vec3 calcNormal3D(vec3 p, PetPose3D pose) {
  const float e = 0.0022;
  const vec2 k = vec2(1.0, -1.0);
  return normalize(
    k.xyy * mapPet3D(p + k.xyy * e, pose).d +
    k.yyx * mapPet3D(p + k.yyx * e, pose).d +
    k.yxy * mapPet3D(p + k.yxy * e, pose).d +
    k.xxx * mapPet3D(p + k.xxx * e, pose).d
  );
}

float calcSoftShadow3D(vec3 ro, vec3 rd, float mint, float maxt, float k, PetPose3D pose) {
  float res = 1.0;
  float t = mint;
  for (int i = 0; i < 20; ++i) {
    float h = mapPet3D(ro + rd * t, pose).d;
    res = min(res, k * h / t);
    if (res < 0.02) return 0.02;
    t += clamp(h, 0.03, 0.22);
    if (t > maxt) break;
  }
  return clamp(res, 0.0, 1.0);
}

vec3 acesFilm(vec3 x) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 linearToSrgb(vec3 c) {
  return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

float calcAO3D(vec3 p, vec3 n, PetPose3D pose) {
  float occ = 0.0;
  float sca = 1.0;
  for (int i = 0; i < 4; ++i) {
    float h = 0.022 + 0.078 * float(i);
    float d = mapPet3D(p + h * n, pose).d;
    occ += (h - d) * sca;
    sca *= 0.65;
  }
  return clamp(1.0 - 2.65 * occ, 0.16, 1.0);
}

vec4 shadePet3D() {
  PetPose3D pose;
  pose.id     = int(vColor.x + 0.5);
  pose.time   = vColor.y;
  pose.yaw    = vRadii.x;
  pose.tilt   = vRadii.y;
  pose.hopY   = vRadii.z;
  pose.blink  = vRadii.w;
  pose.breath = vColor2.x;
  pose.wag    = vColor2.y;

  float hoverGlow = vParams.y;
  float isDark    = vParams.z;

  // Map quad UV [0,1] -> portrait camera coordinates [-1, 1] (y up)
  vec2 uv = (vLocalUv * 2.0 - 1.0) * vec2(1.0, -1.0);

  // Portrait telephoto studio camera (~85mm lens feel, zero wide-angle distortion)
  vec3 ro = vec3(0.0, 0.14, 3.05);
  vec3 ta = vec3(0.0, -0.05, 0.0);
  vec3 cw = normalize(ta - ro);
  vec3 cu = normalize(cross(cw, vec3(0.0, 1.0, 0.0)));
  vec3 cv = cross(cu, cw);
  vec3 rd = normalize(uv.x * cu + uv.y * cv + 2.45 * cw);

  // Fast bounding-sphere early-out (radius 1.18 around origin)
  float b = dot(ro, rd);
  float c = dot(ro, ro) - 1.24 * 1.24;
  float disc = b * b - c;

  // Studio 3-point lighting directions
  vec3 keyDir  = normalize(vec3(-0.62, 0.85, 0.72));
  vec3 fillDir = normalize(vec3(0.78, 0.35, 0.52));
  vec3 rimDir  = normalize(vec3(0.32, 0.68, -0.78));

  // Raymarch the 3D figurine
  float t = 0.0;
  float minDist = 10.0;
  float minT = 3.0;
  bool hit = false;
  Hit3D hBest = Hit3D(10.0, vec3(0.0), 0.0);

  if (disc > 0.0) {
    float tStart = max(0.2, -b - sqrt(disc));
    float tEnd   = -b + sqrt(disc);
    t = tStart;
    for (int i = 0; i < 48; ++i) {
      vec3 pos = ro + rd * t;
      Hit3D h = mapPet3D(pos, pose);
      if (h.d < minDist) {
        minDist = h.d;
        minT = t;
      }
      if (h.d < 0.0018 * t) {
        hit = true;
        hBest = h;
        break;
      }
      t += h.d * 0.92;
      if (t > tEnd) break;
    }
  }

  // Analytical raymarched 3D floor contact shadow on invisible table y = -0.765!
  vec4 floorCol = vec4(0.0);
  if (rd.y < -0.01) {
    float tf = (-0.765 - ro.y) / rd.y;
    if (tf > 0.0 && tf < 6.0 && (!hit || tf < t)) {
      vec3 fp = ro + rd * tf;
      float rFloor = length(fp.xz);
      if (rFloor < 0.92) {
        float sh = calcSoftShadow3D(fp + vec3(0.0, 0.015, 0.0), keyDir, 0.03, 1.4, 3.2, pose);
        float aoF = clamp(mapPet3D(fp + vec3(0.0, 0.04, 0.0), pose).d * 2.8, 0.0, 1.0);
        float edgeFade = smoothstep(0.92, 0.45, rFloor);
        float shadowDensity = clamp((1.0 - sh) * 0.42 + (1.0 - aoF) * 0.55, 0.0, 1.0) * edgeFade;
        float maxA = mix(0.32, 0.52, isDark);
        floorCol = vec4(vec3(0.11, 0.075, 0.05), shadowDensity * maxA);
      }
    }
  }

  if (!hit) {
    // Subpixel silhouette anti-aliasing for ultra-crisp 3D edges over transparent background
    float pxWorld = 2.0 / (max(min(vSize.x, vSize.y), 16.0) * 2.45) * minT;
    if (minDist < pxWorld * 1.15 && disc > 0.0) {
      vec3 pos = ro + rd * minT;
      Hit3D hEdge = mapPet3D(pos, pose);
      vec3 n = calcNormal3D(pos, pose);
      float cov = 1.0 - smoothstep(0.0, pxWorld * 1.15, minDist);
      vec3 linAlb = pow(max(hEdge.albedo, vec3(0.001)), vec3(2.2));
      float ndl = clamp((dot(n, keyDir) + 0.35) / 1.35, 0.0, 1.0);
      vec3 edgeLin = linAlb * (vec3(1.12, 0.99, 0.86) * ndl * 1.65 + vec3(0.42, 0.44, 0.48))
                   + mix(linAlb, vec3(1.0, 0.93, 0.82), 0.40) * 0.22;
      vec3 edgeRgb = linearToSrgb(acesFilm(edgeLin * 0.88));
      // Blend anti-aliased edge over floor contact shadow
      vec4 fg = vec4(edgeRgb, cov);
      float outA = fg.a + floorCol.a * (1.0 - fg.a);
      vec3 outRgb = (outA > 0.001) ? (fg.rgb * fg.a + floorCol.rgb * floorCol.a * (1.0 - fg.a)) / outA : vec3(0.0);
      return vec4(outRgb, outA);
    }
    return floorCol;
  }

  vec3 pos = ro + rd * t;
  vec3 n = calcNormal3D(pos, pose);
  vec3 v = -rd;

  // Tactile needle-felted wool micro-fiber bump perturbation on MAT_WOOL!
  float woolTex = 0.0;
  if (hBest.mat < 0.5) {
    float n1 = noise3(pos * 44.0);
    float n2 = noise3(pos * 88.0 + vec3(4.1, 2.7, 8.3));
    woolTex = (n1 * 0.65 + n2 * 0.35) - 0.5;
    vec3 gradF = vec3(
      noise3((pos + vec3(0.011, 0.0, 0.0)) * 50.0) - n1,
      noise3((pos + vec3(0.0, 0.011, 0.0)) * 50.0) - n1,
      noise3((pos + vec3(0.0, 0.0, 0.011)) * 50.0) - n1
    );
    n = normalize(n + gradF * 0.42);
  }

  float ao  = calcAO3D(pos, n, pose);
  float sh  = calcSoftShadow3D(pos + n * 0.012, keyDir, 0.025, 1.5, 5.2, pose);
  float ndv = clamp(dot(n, v), 0.0, 1.0);
  float fresnel = pow(1.0 - ndv, 3.0);

  // Physically based LINEAR color space shading -> ACES Filmic -> sRGB output!
  vec3 albedo = pow(max(hBest.albedo, vec3(0.001)), vec3(2.2));
  if (hBest.mat < 0.5) {
    // Subtle multi-tone merino wool strand depth
    albedo *= (1.0 + woolTex * 0.22);
  } else if (abs(hBest.mat - 5.0) < 0.5) {
    // Fine carved oak wood / ceramic subtle grain
    float grain = sin((pos.y * 28.0 + noise3(pos * 12.0) * 3.0)) * 0.06;
    albedo *= (1.0 + grain);
  }

  // Wrap diffuse lighting (warm studio softbox key + sky fill + oak desk bounce)
  float wrap = (hBest.mat < 1.5 || abs(hBest.mat - 3.0) < 0.5) ? 0.36 : 0.12;
  float diffKey  = clamp((dot(n, keyDir) + wrap) / (1.0 + wrap), 0.0, 1.0) * (0.28 + 0.72 * sh);
  float diffFill = clamp(dot(n, fillDir) * 0.5 + 0.5, 0.0, 1.0);
  float bounce   = clamp(-n.y * 0.5 + 0.5, 0.0, 1.0);
  float rimLight = pow(1.0 - ndv, 2.2) * clamp(dot(n, rimDir) * 0.65 + 0.55, 0.0, 1.0);

  vec3 lit = vec3(0.0);
  if (abs(hBest.mat - 2.0) < 0.5) {
    // Glossy obsidian glass doll eye: rich jet-black depth + crisp studio window reflections!
    vec3 refl = reflect(-v, n);
    float softbox1 = smoothstep(0.91, 0.982, dot(refl, normalize(vec3(-0.46, 0.60, 0.65))));
    float softbox2 = smoothstep(0.955, 0.992, dot(refl, normalize(vec3(0.52, -0.32, 0.78))));
    float warmIris = pow(clamp(dot(n, normalize(vec3(0.0, -0.58, 0.80))), 0.0, 1.0), 3.0);
    lit = vec3(0.004, 0.003, 0.002) + vec3(0.055, 0.024, 0.010) * warmIris
        + vec3(2.2, 2.15, 2.05) * (softbox1 * 1.25 + softbox2 * 0.55)
        + vec3(0.55, 0.52, 0.48) * fresnel * 0.30;
  } else if (abs(hBest.mat - 4.0) < 0.5) {
    // Brushed antique brass (Moss's spectacles, Scout's buckle, Pebble's shield rivet)
    vec3 halfK = normalize(keyDir + v);
    vec3 halfF = normalize(fillDir + v);
    float specK = pow(max(dot(n, halfK), 0.0), 40.0);
    float specF = pow(max(dot(n, halfF), 0.0), 18.0);
    lit = albedo * (0.20 * ao + 1.45 * diffKey + 0.45 * diffFill)
        + mix(albedo, vec3(1.0, 0.94, 0.76), 0.45) * (specK * 2.4 + specF * 0.65 + fresnel * 0.55);
  } else {
    // Tactile wool felt (0), matte vinyl skin (1), translucent sprout leaf (3), carved wood/ceramic (5)
    vec3 keyCol  = vec3(1.14, 1.00, 0.86);
    vec3 fillCol = vec3(0.76, 0.86, 1.04);
    vec3 bncCol  = vec3(1.04, 0.82, 0.58);

    lit = albedo * (
      keyCol  * diffKey  * 2.05 +
      fillCol * diffFill * 0.50 * ao +
      bncCol  * bounce   * 0.24 * ao +
      vec3(0.13, 0.115, 0.10) * ao
    );

    if (hBest.mat < 0.5) {
      // Velvet / needle-felted wool Charlie rim sheen (plush peach-fuzz halo in 3D!)
      float sheenBoost = mix(0.28, 0.48, isDark);
      lit += mix(albedo, vec3(1.0, 0.92, 0.82), 0.42) * (rimLight * sheenBoost + fresnel * 0.14);
    } else if (abs(hBest.mat - 3.0) < 0.5) {
      // Translucent backlit green leaf subsurface scattering + waxy dew specular
      float sss = pow(clamp(dot(v, -keyDir) * 0.5 + 0.5, 0.0, 1.0), 2.0) * 0.36;
      float spec = pow(max(dot(n, normalize(keyDir + v)), 0.0), 30.0);
      lit += vec3(0.32, 0.72, 0.14) * sss + vec3(1.0, 0.98, 0.90) * spec * 0.48;
    } else if (abs(hBest.mat - 5.0) < 0.5) {
      // Carved wood / matte ceramic / leather satin clearcoat
      float spec = pow(max(dot(n, normalize(keyDir + v)), 0.0), 24.0);
      lit += vec3(1.0, 0.94, 0.84) * (spec * 0.46 + fresnel * 0.14);
    } else {
      // Soft-touch vinyl/skin satin glow + subsurface warmth
      float spec = pow(max(dot(n, normalize(keyDir + v)), 0.0), 16.0);
      lit += vec3(1.0, 0.82, 0.70) * (fresnel * 0.10 + spec * 0.18);
    }
  }

  // Gentle interactive warmth lift when hovered/petted
  lit += vec3(0.055, 0.04, 0.025) * hoverGlow;

  // ACES Filmic tone mapping -> sRGB output (main() linearizes if swapchain is sRGB)
  vec3 srgb = linearToSrgb(acesFilm(lit * 0.88));
  return vec4(srgb, 1.0);
}

// ============================================================================
// Main Fragment Entry Point
// ============================================================================

void main() {
  if (gl_FragCoord.x < vClip.x || gl_FragCoord.y < vClip.y ||
      gl_FragCoord.x > vClip.x + vClip.z || gl_FragCoord.y > vClip.y + vClip.w) {
    discard;
  }

  int kind = int(vParams.x + 0.5);
  vec4 col = vec4(0.0);

  if (kind == 0 || kind == 3) {
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
      vec4 b = vColor2;
      vec3 rgb = mix(fillCol.rgb, b.rgb, borderFactor * b.a);
      float a = mix(fillCol.a, max(fillCol.a, b.a), borderFactor);
      col = vec4(rgb, a * outerAlpha);
    } else {
      col = vec4(fillCol.rgb, fillCol.a * outerAlpha);
    }
  } else if (kind == 1) {
    vec2 uv = mix(vUv.xy, vUv.zw, vLocalUv);
    float cov = texture(uGlyphAtlas, uv).r;
    if (cov <= 0.002) {
      discard;
    }
    float gamma = (vParams.z > 0.01) ? vParams.z : 0.88;
    cov = pow(cov, gamma);
    col = vec4(vColor.rgb, vColor.a * cov);
  } else if (kind == 2) {
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
    col = shadePet3D();
  } else {
    discard;
  }

  col.a *= vParams.w;
  if (col.a <= 0.002) {
    discard;
  }

  if (pc.viewport.z > 0.5) {
    col.rgb = srgbToLinear(col.rgb);
  }
  outColor = col;
}
