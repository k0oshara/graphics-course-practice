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
#include <fstream>
#include <chrono>
#include <vector>
#include <random>
#include <map>
#include <cmath>

#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/compatibility.hpp>

#include "gltf_loader.hpp"
#include "stb_image.h"

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

uniform mat4x3 bones[100];

layout (location = 0) in vec3 in_position;
layout (location = 1) in vec3 in_normal;
layout (location = 2) in vec2 in_texcoord;
layout (location = 3) in ivec4 in_joints;
layout (location = 4) in vec4  in_weights;

out vec3 normal;
out vec2 texcoord;
out vec4 weights;

void main()
{

    mat4x3 average =
        bones[in_joints.x] * in_weights.x +
        bones[in_joints.y] * in_weights.y +
        bones[in_joints.z] * in_weights.z +
        bones[in_joints.w] * in_weights.w;

    vec4 skinned_pos = mat4(average) * vec4(in_position, 1.0);
    vec3 skinned_nor = mat3(average) * in_normal;

    // gl_Position = projection * view * model * vec4(in_position, 1.0);
    // normal = mat3(model) * in_normal;
    gl_Position = projection * view * model * skinned_pos;
    normal = mat3(model) * skinned_nor;
    texcoord = in_texcoord;
    weights = in_weights;
}
)";

const char fragment_shader_source[] =
R"(#version 330 core

uniform sampler2D albedo;
uniform vec4 color;
uniform int use_texture;

uniform vec3 light_direction;

layout (location = 0) out vec4 out_color;

in vec3 normal;
in vec2 texcoord;
in vec4 weights;

void main()
{
    vec4 albedo_color;

    if (use_texture == 1)
        albedo_color = texture(albedo, texcoord);
    else
        albedo_color = color;

    float ambient = 0.4;
    float diffuse = max(0.0, dot(normalize(normal), light_direction));

    out_color = vec4(albedo_color.rgb * (ambient + diffuse), albedo_color.a);
    // out_color = vec4(weights.rgb, 1.0);
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

template <typename ... Shaders>
GLuint create_program(Shaders ... shaders)
{
    GLuint result = glCreateProgram();
    (glAttachShader(result, shaders), ...);
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
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 8);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window * window = SDL_CreateWindow("Graphics course practice 11",
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
    GLuint albedo_location = glGetUniformLocation(program, "albedo");
    GLuint color_location = glGetUniformLocation(program, "color");
    GLuint use_texture_location = glGetUniformLocation(program, "use_texture");
    GLuint light_direction_location = glGetUniformLocation(program, "light_direction");
    GLuint bones_location = glGetUniformLocation(program, "bones");

    const std::string project_root = PROJECT_ROOT;
    const std::string model_path = project_root + "/dancing/dancing.gltf";

    auto const input_model = load_gltf(model_path);

    // auto it_anim = input_model.animations.find("hip-hop");
    // if (it_anim == input_model.animations.end())
    //     throw std::runtime_error("Animation 'hip-hop' not found");
    // auto const & animation = it_anim->second;

    auto it_hip = input_model.animations.find("hip-hop");
    auto it_rum = input_model.animations.find("rumba");
    auto it_fla = input_model.animations.find("flair");
    if (it_hip == input_model.animations.end())
        throw std::runtime_error("Animation 'hip-hop' not found");
    if (it_rum == input_model.animations.end())
        throw std::runtime_error("Animation 'rumba' not found");
    if (it_fla == input_model.animations.end())
        throw std::runtime_error("Animation 'flair' not found");

    std::string current_anim_name = "hip-hop";
    std::string from_anim_name = current_anim_name;
    std::string to_anim_name = current_anim_name;
    auto const * current_anim = &it_hip->second;
    auto const * from_anim = current_anim;
    auto const * to_anim = current_anim;
    float blend_elapsed = 0.f;
    float blend_duration = 0.35f;
    bool blending = false;

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, input_model.buffer.size(), input_model.buffer.data(), GL_STATIC_DRAW);

    struct mesh
    {
        GLuint vao;
        gltf_model::accessor indices;
        gltf_model::material material;
    };

    auto setup_attribute = [](int index, gltf_model::accessor const & accessor, bool integer = false)
    {
        glEnableVertexAttribArray(index);
        if (integer)
            glVertexAttribIPointer(index, accessor.size, accessor.type, 0, reinterpret_cast<void *>(accessor.view.offset));
        else
            glVertexAttribPointer(index, accessor.size, accessor.type, GL_FALSE, 0, reinterpret_cast<void *>(accessor.view.offset));
    };

    std::vector<mesh> meshes;
    for (auto const & mesh : input_model.meshes)
    {
        for (auto const & primitive : mesh.primitives)
        {
            auto & result = meshes.emplace_back();
            glGenVertexArrays(1, &result.vao);
            glBindVertexArray(result.vao);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, vbo);
            result.indices = primitive.indices;

            setup_attribute(0, primitive.position);
            setup_attribute(1, primitive.normal);
            setup_attribute(2, primitive.texcoord);
            setup_attribute(3, primitive.joints, true);
            setup_attribute(4, primitive.weights);

            result.material = primitive.material;
        }
    }

    std::map<std::string, GLuint> textures;
    for (auto const & mesh : meshes)
    {
        if (!mesh.material.texture_path) continue;
        if (textures.contains(*mesh.material.texture_path)) continue;

        auto path = std::filesystem::path(model_path).parent_path() / *mesh.material.texture_path;

        int width, height, channels;
        auto data = stbi_load(path.c_str(), &width, &height, &channels, 4);
        assert(data);

        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        stbi_image_free(data);

        textures[*mesh.material.texture_path] = texture;
    }

    auto last_frame_start = std::chrono::high_resolution_clock::now();

    float time = 0.f;

    std::map<SDL_Keycode, bool> button_down;

    float view_angle = 0.f;
    float camera_distance = 1.5f;

    float camera_rotation = 0.f;
    float camera_height = 1.f;

    bool paused = false;

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
            if (event.key.keysym.sym == SDLK_1 || event.key.keysym.sym == SDLK_2 || event.key.keysym.sym == SDLK_3)
            {
                std::string req_name;
                auto const * req_anim = (decltype(current_anim))nullptr;
                if (event.key.keysym.sym == SDLK_1) { req_name = "hip-hop"; req_anim = &it_hip->second; }
                if (event.key.keysym.sym == SDLK_2) { req_name = "rumba";   req_anim = &it_rum->second; }
                if (event.key.keysym.sym == SDLK_3) { req_name = "flair";   req_anim = &it_fla->second; }

                if (req_anim && req_name != to_anim_name)
                {
                    from_anim = blending ? to_anim : current_anim;
                    from_anim_name = blending ? to_anim_name : current_anim_name;
                    to_anim = req_anim;
                    to_anim_name = req_name;
                    blend_elapsed = 0.f;
                    blending = true;
                }
            }
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
            camera_distance -= 3.f * dt;
        if (button_down[SDLK_DOWN])
            camera_distance += 3.f * dt;

        if (button_down[SDLK_a])
            camera_rotation -= 2.f * dt;
        if (button_down[SDLK_d])
            camera_rotation += 2.f * dt;

        if (button_down[SDLK_w])
            view_angle -= 2.f * dt;
        if (button_down[SDLK_s])
            view_angle += 2.f * dt;

        if (blending && !paused)
        {
            blend_elapsed += dt;
            if (blend_elapsed >= blend_duration)
            {
                blending = false;
                current_anim = to_anim;
                current_anim_name = to_anim_name;
                from_anim = current_anim;
                from_anim_name = current_anim_name;
            }
        }

        glClearColor(0.8f, 0.8f, 1.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        float near = 0.1f;
        float far = 100.f;

        glm::mat4 model = glm::scale(glm::mat4(1.f), glm::vec3(0.01f));

        glm::mat4 view(1.f);
        view = glm::translate(view, {0.f, 0.f, -camera_distance});
        view = glm::rotate(view, view_angle, {1.f, 0.f, 0.f});
        view = glm::rotate(view, camera_rotation, {0.f, 1.f, 0.f});
        view = glm::translate(view, {0.f, -camera_height, 0.f});

        glm::mat4 projection = glm::perspective(glm::pi<float>() / 2.f, (1.f * width) / height, near, far);

        glm::vec3 camera_position = (glm::inverse(view) * glm::vec4(0.f, 0.f, 0.f, 1.f)).xyz();

        glm::vec3 light_direction = glm::normalize(glm::vec3(1.f, 2.f, 3.f));

        // float scale = 0.75f + std::cos(time) * 0.25f;
        // std::vector<glm::mat4x3> bone_matrices(input_model.bones.size(), glm::mat4x3(scale));

        // float anim_t = 0.f;
        // float anim_t = (animation.max_time > 0.f) ? std::fmod(time, animation.max_time) : 0.f;

        float blend_k = 0.f;
        if (blending)
            blend_k = (blend_duration > 0.f) ? glm::clamp(blend_elapsed / blend_duration, 0.f, 1.f) : 1.f;

        auto const * a0 = blending ? from_anim : current_anim;
        auto const * a1 = blending ? to_anim   : current_anim;

        float t0 = (a0->max_time > 0.f) ? std::fmod(time, a0->max_time) : 0.f;
        float t1 = (a1->max_time > 0.f) ? std::fmod(time, a1->max_time) : 0.f;

        std::vector<glm::mat4> bone_global(input_model.bones.size(), glm::mat4(1.f));
        std::vector<char> bone_ready(input_model.bones.size(), 0);
        std::vector<glm::mat4x3> bone_matrices(input_model.bones.size());

        auto sample_trs = [&](decltype(a0) anim, int i, float t, glm::vec3 & tr, glm::quat & rot, glm::vec3 & sca)
        {
            tr = glm::vec3(0.f);
            rot = glm::quat(1.f, 0.f, 0.f, 0.f);
            sca = glm::vec3(1.f);
            if (i < (int)anim->bones.size())
            {
                tr = anim->bones[i].translation(t);
                rot = anim->bones[i].rotation(t);
                sca = anim->bones[i].scale(t);
            }
        };

        auto get_global = [&](auto && self, int i) -> glm::mat4
        {
            if (bone_ready[i]) return bone_global[i];

            // glm::vec3 t(0.f);
            // glm::quat r(1.f, 0.f, 0.f, 0.f);
            // glm::vec3 s(1.f);
            glm::vec3 t_a, t_b, t;
            glm::quat r_a, r_b, r;
            glm::vec3 s_a, s_b, s;

            sample_trs(a0, i, t0, t_a, r_a, s_a);
            sample_trs(a1, i, t1, t_b, r_b, s_b);

            t = glm::lerp(t_a, t_b, blend_k);
            r = glm::slerp(r_a, r_b, blend_k);
            s = glm::lerp(s_a, s_b, blend_k);

            // if (i < (int)animation.bones.size())
            // {
            //     t = animation.bones[i].translation(anim_t);
            //     r = animation.bones[i].rotation(anim_t);
            //     s = animation.bones[i].scale(anim_t);
            // }

            glm::mat4 transform =
                glm::translate(glm::mat4(1.f), t) *
                glm::toMat4(r) *
                glm::scale(glm::mat4(1.f), s);

            int parent = input_model.bones[i].parent;
            if (parent != -1) transform = self(self, parent) * transform;

            bone_global[i] = transform;
            bone_ready[i] = 1;
            return transform;
        };

        for (int i = 0; i < (int)input_model.bones.size(); ++i)
        {
            glm::mat4 transform = get_global(get_global, i);
            bone_matrices[i] = glm::mat4x3(transform);
        }

        for (int i = 0; i < (int)input_model.bones.size(); ++i)
        {
            glm::mat4 final_m = glm::mat4(bone_matrices[i]) * input_model.bones[i].inverse_bind_matrix;
            bone_matrices[i] = glm::mat4x3(final_m);
        }

        glUseProgram(program);
        glUniformMatrix4fv(model_location, 1, GL_FALSE, reinterpret_cast<float *>(&model));
        glUniformMatrix4fv(view_location, 1, GL_FALSE, reinterpret_cast<float *>(&view));
        glUniformMatrix4fv(projection_location, 1, GL_FALSE, reinterpret_cast<float *>(&projection));
        glUniform3fv(light_direction_location, 1, reinterpret_cast<float *>(&light_direction));

        GLsizei bones_count = (GLsizei)bone_matrices.size();
        if (bones_count > 100) bones_count = 100;
        glUniformMatrix4x3fv(bones_location, bones_count, GL_FALSE, reinterpret_cast<const float *>(bone_matrices.data()));

        auto draw_meshes = [&](bool transparent)
        {
            for (auto const & mesh : meshes)
            {
                if (mesh.material.transparent != transparent)
                    continue;

                if (mesh.material.two_sided)
                    glDisable(GL_CULL_FACE);
                else
                    glEnable(GL_CULL_FACE);

                if (transparent)
                    glEnable(GL_BLEND);
                else
                    glDisable(GL_BLEND);

                if (mesh.material.texture_path)
                {
                    glBindTexture(GL_TEXTURE_2D, textures[*mesh.material.texture_path]);
                    glUniform1i(use_texture_location, 1);
                }
                else if (mesh.material.color)
                {
                    glUniform1i(use_texture_location, 0);
                    glUniform4fv(color_location, 1, reinterpret_cast<const float *>(&(*mesh.material.color)));
                }
                else
                    continue;

                glBindVertexArray(mesh.vao);
                glDrawElements(GL_TRIANGLES, mesh.indices.count, mesh.indices.type, reinterpret_cast<void *>(mesh.indices.view.offset));
            }
        };

        draw_meshes(false);
        glDepthMask(GL_FALSE);
        draw_meshes(true);
        glDepthMask(GL_TRUE);

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
