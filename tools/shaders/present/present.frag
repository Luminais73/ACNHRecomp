#version 450
// Host pixel -> the picture: `rot` turns host uv into upright uv (the swapchain's preTransform undone), `box` is where
// the picture sits in the upright target (uv, fit with bars), `crop` the part of the texture shown. Alpha is 1.
// Over it, the touch controls: `n` circles (x, y, radius in target heights, opacity) in upright uv. `cell` is a coarse
// 8x4 grid over the upright target, a bit per circle that reaches the cell: a pixel tests only those (every pixel
// testing every circle was ~8 ms a frame on an Adreno 650 at 2400x1080).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform Overlay {
    vec4 shape[32];
    uvec4 cell[8];
} ov;
layout(push_constant) uniform P {
    vec4 rot;   // mat2 columns (x, y) (z, w)
    vec4 off;   // xy: rot offset; z: number of circles; w: target width / height (upright)
    vec4 box;   // x0 y0 w h
    vec4 crop;  // u0 v0 du dv
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    vec2 b = (up - p.box.xy) / p.box.zw;
    vec3 c = vec3(0.0);
    if (all(greaterThanEqual(b, vec2(0.0))) && all(lessThanEqual(b, vec2(1.0))))
        c = texture(src, p.crop.xy + b * p.crop.zw).rgb;
    if (p.off.z > 0.0) {
        ivec2 g = clamp(ivec2(up * vec2(8.0, 4.0)), ivec2(0), ivec2(7, 3));
        int ci = g.y * 8 + g.x;
        uint m = ov.cell[ci >> 2][ci & 3];
        while (m != 0u) {
            int i = findLSB(m);
            m &= m - 1u;
            vec4 s = ov.shape[i];
            float d = length(vec2((up.x - s.x) * p.off.w, up.y - s.y));
            if (d < s.z)
                c = mix(c, vec3(1.0), s.w);
            else if (d < s.z + 0.005)
                c = mix(c, vec3(1.0), min(1.0, s.w * 3.0));
        }
    }
    color = vec4(c, 1.0);
}
