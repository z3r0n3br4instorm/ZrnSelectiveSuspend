#version 450
// One unit quad, placed by the per-frame uniform block.
layout(location = 0) in vec2 in_pos;
layout(location = 0) out vec2 out_uv;

layout(set = 0, binding = 0) uniform Frame {
    vec4 stamp_rect;  // x, y, w, h in clip space
    vec4 stamp_color;
    vec4 badge_rect;
    vec4 badge_tint;
} frame;

layout(push_constant) uniform Draw {
    int mode;  // 0 stamp, 1 full-screen history, 2 textured badge
} draw;

void main()
{
    vec4 rect = draw.mode == 0 ? frame.stamp_rect
              : draw.mode == 1 ? vec4(-1.0, -1.0, 2.0, 2.0)
                               : frame.badge_rect;
    out_uv = in_pos;
    gl_Position = vec4(rect.xy + in_pos * rect.zw, draw.mode == 2 ? 0.25 : 0.75, 1.0);
}
