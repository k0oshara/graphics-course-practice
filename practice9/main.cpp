#ifdef WIN32
#include <SDL.h>
#undef main
#else
#include <SDL2/SDL.h>
#endif

#include <GL/glew.h>

#include <string_view>
#include <stdexcept>
#include <iostream>
#include <chrono>
#include <vector>
#include <map>
#include <cmath>
#include <fstream>
#include <sstream>

#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtx/string_cast.hpp>

#include "obj_parser.hpp"

std::string to_string(std::string_view str)
{
    return std::string(str.begin(), str.end());
}

void sdl2_fail(std::string_view message)
{
    throw std::runtime_error(to_string(message) + SDL_GetError());
}

void glew_fail(std::string_view message, GLenum error)
{
    throw std::runtime_error(to_string(message) + reinterpret_cast<const char *>(glewGetErrorString(error)));
}

const char vertex_shader_source[] =
R"(#version 330 core

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

layout (location = 0) in vec3 in_position;
layout (location = 1) in vec3 in_normal;

out vec3 position;
out vec3 normal;

void main()
{
    gl_Position = projection * view * model * vec4(in_position, 1.0);
    position = (model * vec4(in_position, 1.0)).xyz;
    normal = normalize((model * vec4(in_normal, 0.0)).xyz);
}
)";

const char fragment_shader_source[] =
R"(#version 330 core

uniform vec3 ambient;

uniform vec3 light_direction;
uniform vec3 light_color;

uniform mat4 transform;

uniform sampler2D shadow_map;
uniform float shadow_bias;

in vec3 position;
in vec3 normal;

layout (location = 0) out vec4 out_color;

void main()
{
    vec4 shadow_pos = transform * vec4(position, 1.0);
    shadow_pos /= shadow_pos.w;
    shadow_pos = shadow_pos * 0.5 + vec4(0.5);

    // bool in_shadow_texture = (shadow_pos.x > 0.0) && (shadow_pos.x < 1.0) && (shadow_pos.y > 0.0) && (shadow_pos.y < 1.0) && (shadow_pos.z > 0.0) && (shadow_pos.z < 1.0);
    // float shadow_factor = 1.0;
    // if (in_shadow_texture)
    //     shadow_factor = (texture(shadow_map, shadow_pos.xy).r + shadow_bias < shadow_pos.z) ? 0.0 : 1.0;

    float shadow_factor = 1.0;

    if (all(greaterThanEqual(shadow_pos.xy, vec2(0.0))) &&
        all(lessThanEqual(shadow_pos.xy, vec2(1.0))) &&
        shadow_pos.z >= 0.0 && shadow_pos.z <= 1.0)
    {
        vec2 data = texture(shadow_map, shadow_pos.xy).rg;
        float mu = data.r;
        float sigma2 = data.g - mu * mu; // Var[z] = E[z^2] - E[z]^2
        sigma2 = max(sigma2, 0.00002);

        float z = shadow_pos.z - shadow_bias;

        shadow_factor = (z <= mu) ? 1.0 : sigma2 / (sigma2 + (z - mu) * (z - mu));

        const float delta = 0.125;
        shadow_factor = (shadow_factor <= delta) ? 0.0 : min((shadow_factor - delta) / (1.0 - delta), 1.0);
    }

    vec3 albedo = vec3(1.0, 1.0, 1.0);

    vec3 light = ambient;
    light += light_color * max(0.0, dot(normal, light_direction)) * shadow_factor;
    vec3 color = albedo * light;

    out_color = vec4(color, 1.0);
}
)";

const char debug_vertex_shader_source[] =
R"(#version 330 core

vec2 vertices[6] = vec2[6](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

out vec2 texcoord;

void main()
{
    vec2 position = vertices[gl_VertexID];
    gl_Position = vec4(position * 0.25 + vec2(-0.75, -0.75), 0.0, 1.0);
    texcoord = position * 0.5 + vec2(0.5);
}
)";

const char debug_fragment_shader_source[] =
R"(#version 330 core

uniform sampler2D shadow_map;

in vec2 texcoord;

layout (location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(texture(shadow_map, texcoord));
}
)";

const char shadow_vertex_shader_source[] =
R"(#version 330 core

uniform mat4 model;
uniform mat4 transform;

layout (location = 0) in vec3 in_position;

void main()
{
    gl_Position = transform * model * vec4(in_position, 1.0);
}
)";

const char shadow_fragment_shader_source[] =
R"(#version 330 core

out vec4 out_color;

void main()
{
    float z = gl_FragCoord.z;
    float dzdx = dFdx(z);
    float dzdy = dFdy(z);
    float z2 = z * z + 0.25 * (dzdx * dzdx + dzdy * dzdy);
    out_color = vec4(z, z2, 0.0, 0.0);
    // out_color = vec4(z, z * z, 0.0, 0.0);
}
)";

const char blur_vertex_shader_source[] =
R"(#version 330 core

vec2 positions[6] = vec2[6](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);

out vec2 texcoord;

void main()
{
    vec2 pos = positions[gl_VertexID];
    gl_Position = vec4(pos, 0.0, 1.0);
    texcoord = pos * 0.5 + 0.5;
}
)";

const char blur_fragment_shader_source[] =
R"(#version 330 core

uniform sampler2D input_map;
uniform float texel_size;
uniform bool horizontal;

in vec2 texcoord;
out vec4 out_color;

void main()
{
    const float kernel[5] = float[]( 0.06136, 0.24477, 0.38774, 0.24477, 0.06136 );

    vec2 accum = vec2(0.0);
    if (horizontal) {
        for (int i = -2; i <= 2; ++i) {
            vec2 uv = texcoord + vec2(texel_size * float(i), 0.0);
            uv = clamp(uv, vec2(0.0), vec2(1.0));
            accum += texture(input_map, uv).rg * kernel[i + 2];
        }
    } else {
        for (int i = -2; i <= 2; ++i) {
            vec2 uv = texcoord + vec2(0.0, texel_size * float(i));
            uv = clamp(uv, vec2(0.0), vec2(1.0));
            accum += texture(input_map, uv).rg * kernel[i + 2];
        }
    }
    out_color = vec4(accum, 0.0, 0.0);
}
)";

GLuint create_shader(GLenum type, const char * source)
{
    GLuint result = glCreateShader(type);
    glShaderSource(result, 1, &source, nullptr);
    glCompileShader(result);
    GLint status;
    glGetShaderiv(result, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLint info_log_length;
        glGetShaderiv(result, GL_INFO_LOG_LENGTH, &info_log_length);
        std::string info_log(info_log_length, '\0');
        glGetShaderInfoLog(result, info_log.size(), nullptr, info_log.data());
        throw std::runtime_error("Shader compilation failed: " + info_log);
    }
    return result;
}

GLuint create_program(GLuint vertex_shader, GLuint fragment_shader)
{
    GLuint result = glCreateProgram();
    glAttachShader(result, vertex_shader);
    glAttachShader(result, fragment_shader);
    glLinkProgram(result);

    GLint status;
    glGetProgramiv(result, GL_LINK_STATUS, &status);
    if (status != GL_TRUE)
    {
        GLint info_log_length;
        glGetProgramiv(result, GL_INFO_LOG_LENGTH, &info_log_length);
        std::string info_log(info_log_length, '\0');
        glGetProgramInfoLog(result, info_log.size(), nullptr, info_log.data());
        throw std::runtime_error("Program linkage failed: " + info_log);
    }

    return result;
}

int main() try
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        sdl2_fail("SDL_Init: ");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window * window = SDL_CreateWindow("Graphics course practice 9",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        800, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    if (!window)
        sdl2_fail("SDL_CreateWindow: ");

    int width, height;
    SDL_GetWindowSize(window, &width, &height);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
        sdl2_fail("SDL_GL_CreateContext: ");

    if (auto result = glewInit(); result != GLEW_NO_ERROR)
        glew_fail("glewInit: ", result);

    if (!GLEW_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 is not supported");

    auto vertex_shader = create_shader(GL_VERTEX_SHADER, vertex_shader_source);
    auto fragment_shader = create_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    auto program = create_program(vertex_shader, fragment_shader);

    GLuint model_location = glGetUniformLocation(program, "model");
    GLuint view_location = glGetUniformLocation(program, "view");
    GLuint projection_location = glGetUniformLocation(program, "projection");
    GLuint transform_location = glGetUniformLocation(program, "transform");

    GLuint ambient_location = glGetUniformLocation(program, "ambient");
    GLuint light_direction_location = glGetUniformLocation(program, "light_direction");
    GLuint light_color_location = glGetUniformLocation(program, "light_color");

    GLuint shadow_map_location = glGetUniformLocation(program, "shadow_map");
    GLuint shadow_bias_location = glGetUniformLocation(program, "shadow_bias");

    glUseProgram(program);
    glUniform1i(shadow_map_location, 0);
    glUniform1f(shadow_bias_location, 0.005f);

    auto debug_vertex_shader = create_shader(GL_VERTEX_SHADER, debug_vertex_shader_source);
    auto debug_fragment_shader = create_shader(GL_FRAGMENT_SHADER, debug_fragment_shader_source);
    auto debug_program = create_program(debug_vertex_shader, debug_fragment_shader);

    GLuint debug_shadow_map_location = glGetUniformLocation(debug_program, "shadow_map");

    glUseProgram(debug_program);
    glUniform1i(debug_shadow_map_location, 0);

    auto shadow_vertex_shader = create_shader(GL_VERTEX_SHADER, shadow_vertex_shader_source);
    auto shadow_fragment_shader = create_shader(GL_FRAGMENT_SHADER, shadow_fragment_shader_source);
    auto shadow_program = create_program(shadow_vertex_shader, shadow_fragment_shader);

    GLuint shadow_model_location = glGetUniformLocation(shadow_program, "model");
    GLuint shadow_transform_location = glGetUniformLocation(shadow_program, "transform");

    auto blur_vertex_shader = create_shader(GL_VERTEX_SHADER, blur_vertex_shader_source);
    auto blur_fragment_shader = create_shader(GL_FRAGMENT_SHADER, blur_fragment_shader_source);
    auto blur_program = create_program(blur_vertex_shader, blur_fragment_shader);
    GLuint blur_input_location = glGetUniformLocation(blur_program, "input_map");
    GLuint blur_texel_size_location = glGetUniformLocation(blur_program, "texel_size");
    GLuint blur_horizontal_location = glGetUniformLocation(blur_program, "horizontal");

    glUseProgram(blur_program);
    glUniform1i(blur_input_location, 0);

    std::string project_root = PROJECT_ROOT;
    std::string scene_path = project_root + "/bunny.obj";
    obj_data scene = parse_obj(scene_path);

    glm::vec3 scene_min(std::numeric_limits<float>::max());
    glm::vec3 scene_max(std::numeric_limits<float>::lowest());

    for (const auto& v : scene.vertices) {
        glm::vec3 pos(v.position[0], v.position[1], v.position[2]);
        scene_min = glm::min(scene_min, pos);
        scene_max = glm::max(scene_max, pos);
    }
    glm::vec3 scene_center = (scene_min + scene_max) * 0.5f;

    GLuint vao, vbo, ebo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, scene.vertices.size() * sizeof(scene.vertices[0]), scene.vertices.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, scene.indices.size() * sizeof(scene.indices[0]), scene.indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(obj_data::vertex), (void*)(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(obj_data::vertex), (void*)(12));

    GLuint debug_vao;
    glGenVertexArrays(1, &debug_vao);

    GLsizei shadow_map_resolution = 1024;

    GLuint shadow_map;
    glGenTextures(1, &shadow_map);
    glBindTexture(GL_TEXTURE_2D, shadow_map);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, shadow_map_resolution, shadow_map_resolution, 0, GL_RG, GL_FLOAT, nullptr);

    GLuint shadow_depth_rb;
    glGenRenderbuffers(1, &shadow_depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, shadow_depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, shadow_map_resolution, shadow_map_resolution);

    GLuint shadow_fbo;
    glGenFramebuffers(1, &shadow_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, shadow_fbo);
    // glFramebufferTexture(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, shadow_map, 0);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shadow_map, 0);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, shadow_depth_rb);
    GLenum draw_buffers[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw_buffers);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Incomplete framebuffer!");
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    GLuint temp_blurred_shadow_map;
    glGenTextures(1, &temp_blurred_shadow_map);
    glBindTexture(GL_TEXTURE_2D, temp_blurred_shadow_map);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, shadow_map_resolution, shadow_map_resolution, 0, GL_RG, GL_FLOAT, nullptr);

    GLuint blurred_shadow_map;
    glGenTextures(1, &blurred_shadow_map);
    glBindTexture(GL_TEXTURE_2D, blurred_shadow_map);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, shadow_map_resolution, shadow_map_resolution, 0, GL_RG, GL_FLOAT, nullptr);

    GLuint blur_fbo_horizontal;
    glGenFramebuffers(1, &blur_fbo_horizontal);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blur_fbo_horizontal);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, temp_blurred_shadow_map, 0);
    glDrawBuffers(1, draw_buffers);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Horizontal blur FBO incomplete!");

    GLuint blur_fbo_vertical;
    glGenFramebuffers(1, &blur_fbo_vertical);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blur_fbo_vertical);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurred_shadow_map, 0);
    glDrawBuffers(1, draw_buffers);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("Vertical blur FBO incomplete!");

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glUseProgram(blur_program);
    glUniform1f(blur_texel_size_location, 1.0f / shadow_map_resolution);

    auto last_frame_start = std::chrono::high_resolution_clock::now();

    float time = 0.f;
    bool paused = false;

    std::map<SDL_Keycode, bool> button_down;

    float view_elevation = glm::radians(45.f);
    float view_azimuth = 0.f;
    float camera_distance = 1.5f;
    bool running = true;
    while (running)
    {
        for (SDL_Event event; SDL_PollEvent(&event);) switch (event.type)
        {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_WINDOWEVENT: switch (event.window.event)
            {
            case SDL_WINDOWEVENT_RESIZED:
                width = event.window.data1;
                height = event.window.data2;
                glViewport(0, 0, width, height);
                break;
            }
            break;
        case SDL_KEYDOWN:
            button_down[event.key.keysym.sym] = true;

            if (event.key.keysym.sym == SDLK_SPACE)
                paused = !paused;

            break;
        case SDL_KEYUP:
            button_down[event.key.keysym.sym] = false;
            break;
        }

        if (!running)
            break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - last_frame_start).count();
        last_frame_start = now;
        if (!paused)
            time += dt;

        if (button_down[SDLK_UP])
            camera_distance -= 1.f * dt;
        if (button_down[SDLK_DOWN])
            camera_distance += 1.f * dt;

        if (button_down[SDLK_LEFT])
            view_azimuth -= 2.f * dt;
        if (button_down[SDLK_RIGHT])
            view_azimuth += 2.f * dt;

        glm::mat4 model(1.f);

        glm::vec3 light_direction = glm::normalize(glm::vec3(std::cos(time * 0.5f), 1.f, std::sin(time * 0.5f)));

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, shadow_fbo);
        glClearColor(1.0f, 1.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, shadow_map_resolution, shadow_map_resolution);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        // glm::vec3 light_z = -light_direction;
        // glm::vec3 light_x = glm::normalize(glm::cross(light_z, {0.f, 1.f, 0.f}));
        // glm::vec3 light_y = glm::cross(light_x, light_z);
        // float shadow_scale = 2.f;

        // glm::mat4 transform = glm::mat4(1.f);
        // for (size_t i = 0; i < 3; ++i)
        // {
        //     transform[i][0] = shadow_scale * light_x[i];
        //     transform[i][1] = shadow_scale * light_y[i];
        //     transform[i][2] = shadow_scale * light_z[i];
        // }

        glm::vec3 light_z = -light_direction;
        glm::vec3 light_x = glm::normalize(glm::cross(light_z, glm::vec3(0.f, 1.f, 0.f)));
        if (glm::length(light_x) < 1e-6f) {
            light_x = glm::vec3(1.f, 0.f, 0.f);
        }
        glm::vec3 light_y = glm::cross(light_z, light_x);

        std::vector<glm::vec3> corners = {
            glm::vec3(scene_min.x, scene_min.y, scene_min.z),
            glm::vec3(scene_max.x, scene_min.y, scene_min.z),
            glm::vec3(scene_min.x, scene_max.y, scene_min.z),
            glm::vec3(scene_max.x, scene_max.y, scene_min.z),
            glm::vec3(scene_min.x, scene_min.y, scene_max.z),
            glm::vec3(scene_max.x, scene_min.y, scene_max.z),
            glm::vec3(scene_min.x, scene_max.y, scene_max.z),
            glm::vec3(scene_max.x, scene_max.y, scene_max.z)
        };

        float half_width = 0.0f;
        float half_height = 0.0f;
        float half_depth = 0.0f;

        for (const auto& V : corners) {
            glm::vec3 d = V - scene_center;
            half_width = std::max(half_width, std::abs(glm::dot(d, light_x)));
            half_height = std::max(half_height, std::abs(glm::dot(d, light_y)));
            half_depth = std::max(half_depth, std::abs(glm::dot(d, light_z)));
        }

        glm::vec3 light_pos = scene_center - light_z * half_depth;
        glm::mat4 light_view = glm::lookAt(light_pos, scene_center, -light_y);

        glm::mat4 light_proj = glm::ortho(
            -half_width, half_width,
            -half_height, half_height,
            0.0f, 2.0f * half_depth
        );
        glm::mat4 transform = light_proj * light_view;

        glUseProgram(shadow_program);
        glUniformMatrix4fv(shadow_model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        glUniformMatrix4fv(shadow_transform_location, 1, GL_FALSE, reinterpret_cast<float *>(&transform));

        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, scene.indices.size(), GL_UNSIGNED_INT, nullptr);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blur_fbo_horizontal);
        glViewport(0, 0, shadow_map_resolution, shadow_map_resolution);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(blur_program);
        glUniform1i(blur_horizontal_location, true);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadow_map);
        glBindVertexArray(debug_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blur_fbo_vertical);
        glViewport(0, 0, shadow_map_resolution, shadow_map_resolution);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(blur_horizontal_location, false);
        glBindTexture(GL_TEXTURE_2D, temp_blurred_shadow_map);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // glBindTexture(GL_TEXTURE_2D, shadow_map);
        // glGenerateMipmap(GL_TEXTURE_2D);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);

        glClearColor(0.8f, 0.8f, 0.9f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);

        float near = 0.01f;
        float far = 10.f;

        glm::mat4 view(1.f);
        view = glm::translate(view, {0.f, 0.f, -camera_distance});
        view = glm::rotate(view, view_elevation, {1.f, 0.f, 0.f});
        view = glm::rotate(view, view_azimuth, {0.f, 1.f, 0.f});

        glm::mat4 projection = glm::mat4(1.f);
        projection = glm::perspective(glm::pi<float>() / 2.f, (1.f * width) / height, near, far);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, blurred_shadow_map);

        glUseProgram(program);
        glUniformMatrix4fv(model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        glUniformMatrix4fv(view_location, 1, GL_FALSE, reinterpret_cast<float *>(&view));
        glUniformMatrix4fv(projection_location, 1, GL_FALSE, reinterpret_cast<float *>(&projection));
        glUniformMatrix4fv(transform_location, 1, GL_FALSE, reinterpret_cast<float *>(&transform));

        glUniform3f(ambient_location, 0.2f, 0.2f, 0.2f);
        glUniform3fv(light_direction_location, 1, reinterpret_cast<float *>(&light_direction));
        glUniform3f(light_color_location, 0.8f, 0.8f, 0.8f);

        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, scene.indices.size(), GL_UNSIGNED_INT, nullptr);

        glUseProgram(debug_program);
        glBindTexture(GL_TEXTURE_2D, blurred_shadow_map);
        glBindVertexArray(debug_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
}
catch (std::exception const & e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
