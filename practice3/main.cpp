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
#include <random>

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

uniform mat4 view;
uniform float time;

layout (location = 0) in vec2 in_position;
layout (location = 1) in vec4 in_color;
layout (location = 2) in float in_distance;

out vec4 color;
out float distance;

void main()
{
    gl_Position = view * vec4(in_position, 0.0, 1.0);
    color = in_color;
    distance = in_distance + time * 50.0;
}
)";

const char fragment_shader_source[] =
R"(#version 330 core

uniform int dash;
in vec4 color;
in float distance;

layout (location = 0) out vec4 out_color;

void main()
{
    if (dash == 1) {
        float m = mod(distance, 40.0);
        if (m >= 20.0) discard;
    }
    out_color = color;
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

struct vec2
{
    float x;
    float y;
};

struct vertex
{
    vec2 position;
    std::uint8_t color[4];
};

struct bezier_vertex
{
    vec2 position;
    std::uint8_t color[4];
    float dist;
};

vec2 bezier(std::vector<vertex> const & vertices, float t)
{
    std::vector<vec2> points(vertices.size());

    for (std::size_t i = 0; i < vertices.size(); ++i)
        points[i] = vertices[i].position;

    // De Casteljau's algorithm
    for (std::size_t k = 0; k + 1 < vertices.size(); ++k) {
        for (std::size_t i = 0; i + k + 1 < vertices.size(); ++i) {
            points[i].x = points[i].x * (1.f - t) + points[i + 1].x * t;
            points[i].y = points[i].y * (1.f - t) + points[i + 1].y * t;
        }
    }
    return points[0];
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
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    SDL_Window * window = SDL_CreateWindow("Graphics course practice 3",
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

    SDL_GL_SetSwapInterval(0);

    if (auto result = glewInit(); result != GLEW_NO_ERROR)
        glew_fail("glewInit: ", result);

    if (!GLEW_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 is not supported");

    glClearColor(0.8f, 0.8f, 1.f, 0.f);

    auto vertex_shader = create_shader(GL_VERTEX_SHADER, vertex_shader_source);
    auto fragment_shader = create_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    auto program = create_program(vertex_shader, fragment_shader);

    GLuint view_location = glGetUniformLocation(program, "view");
    GLuint dash_location = glGetUniformLocation(program, "dash");
    GLuint time_location = glGetUniformLocation(program, "time");

    // std::vector<vertex> verts = {
    //     {{0.10f, 0.30f}, {0, 0, 1, 1}},
    //     {{0.10f, -0.05f}, {1, 0, 0, 1}},
    //     {{0.50f, -0.05f}, {0, 1, 0, 1}},
    // };

    // std::vector<vertex> verts = {
    //     {{width * 0.52f, height * 0.40f}, {0, 0, 1, 1}},
    //     {{width * 0.52f, height * 0.52f}, {1, 0, 0, 1}},
    //     {{width * 0.72f, height * 0.52f}, {0, 1, 0, 1}},
    // };

    std::vector<vertex> verts;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    int quality = 4;

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    // glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(vertex), verts.data(), GL_STATIC_DRAW);

    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE,
        sizeof(vertex),
        (void*)(offsetof(vertex, position))
    );

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        // 1, 4, GL_UNSIGNED_BYTE, GL_FALSE,
        1, 4, GL_UNSIGNED_BYTE, GL_TRUE,
        sizeof(vertex),
        (void*)(offsetof(vertex, color))
    );

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(
        2, 1, GL_FLOAT, GL_FALSE,
        sizeof(vertex),
        (void*)(offsetof(vertex, color) + 4)
    );
    glVertexAttribDivisor(2, 0);

    GLuint bezier_vao, bezier_vbo;
    glGenVertexArrays(1, &bezier_vao);
    glGenBuffers(1, &bezier_vbo);

    glBindVertexArray(bezier_vao);
    glBindBuffer(GL_ARRAY_BUFFER, bezier_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE,
        sizeof(bezier_vertex),
        (void*)offsetof(vertex, position)
    );

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 4, GL_UNSIGNED_BYTE, GL_TRUE,
        sizeof(bezier_vertex),
        (void*)offsetof(vertex, color)
    );

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(
        2, 1, GL_FLOAT, GL_FALSE,
        sizeof(bezier_vertex),
        (void*)offsetof(bezier_vertex, dist)
    );

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    {
        float v2_y = 0.f;
        const GLintptr offset = static_cast<GLintptr>(sizeof(vertex) * 1 + offsetof(vertex, position) + sizeof(float));
        glGetBufferSubData(GL_ARRAY_BUFFER, offset, sizeof(float), &v2_y);
        std::cout << "v2.y = " << v2_y << std::endl;
    }

    bool verts_updated = false;
    bool bezier_updated = false;

    auto last_frame_start = std::chrono::high_resolution_clock::now();

    float time = 0.f;

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
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.button == SDL_BUTTON_LEFT)
            {
                int mouse_x = event.button.x;
                int mouse_y = event.button.y;

                vertex new_vertex;
                new_vertex.position.x = static_cast<float>(mouse_x);
                new_vertex.position.y = static_cast<float>(mouse_y);

                new_vertex.color[0] = dis(gen);
                new_vertex.color[1] = dis(gen);
                new_vertex.color[2] = dis(gen);
                new_vertex.color[3] = 255;

                verts.push_back(new_vertex);
                verts_updated = true;
                bezier_updated = true;
            }
            else if (event.button.button == SDL_BUTTON_RIGHT)
            {
                if (!verts.empty())
                {
                    verts.pop_back();
                    verts_updated = true;
                    bezier_updated = true;
                }
            }
            break;
        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_LEFT)
            {
                if (quality > 1)
                {
                    quality--;
                    bezier_updated = true;
                }
            }
            else if (event.key.keysym.sym == SDLK_RIGHT)
            {
                quality++;
                bezier_updated = true;
            }
            break;
        }

        if (!running)
            break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - last_frame_start).count();
        last_frame_start = now;
        time += dt;

        if (verts_updated)
        {
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(vertex), verts.data(), GL_DYNAMIC_DRAW);
            verts_updated = false;
        }

        if (bezier_updated && verts.size() >= 2)
        {
            std::vector<bezier_vertex> bezier_verts;
            int segments = (verts.size() - 1) * quality;

            float total_dist = 0.0f;
            vec2 prev_point = bezier(verts, 0.0f);

            for (int i = 0; i <= segments; ++i)
            {
                float t = static_cast<float>(i) / segments;
                vec2 point = bezier(verts, t);

                if (i > 0) {
                    float segment_len = std::hypot(point.x - prev_point.x, point.y - prev_point.y);
                    total_dist += segment_len;
                }

                bezier_vertex v;
                v.position = point;
                v.color[0] = 255;
                v.color[1] = 0;
                v.color[2] = 0;
                v.color[3] = 255;
                v.dist = total_dist;

                bezier_verts.push_back(v);
                prev_point = point;
            }

            glBindBuffer(GL_ARRAY_BUFFER, bezier_vbo);
            glBufferData(GL_ARRAY_BUFFER, bezier_verts.size() * sizeof(bezier_vertex), bezier_verts.data(), GL_DYNAMIC_DRAW);
            bezier_updated = false;
        }

        glClear(GL_COLOR_BUFFER_BIT);

        // float view[16] =
        // {
        //     1.f, 0.f, 0.f, 0.f,
        //     0.f, 1.f, 0.f, 0.f,
        //     0.f, 0.f, 1.f, 0.f,
        //     0.f, 0.f, 0.f, 1.f,
        // };

        float view[16] = {
            2.0f / width, 0.0f, 0.0f, -1.0f,
            0.0f, -2.0f / height, 0.0f, 1.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };

        glUseProgram(program);
        glUniformMatrix4fv(view_location, 1, GL_TRUE, view);
        glUniform1i(dash_location, 0);

        glBindVertexArray(vao);
        // glDrawArrays(GL_TRIANGLES, 0, 3);
        // glBindVertexArray(0);
        glLineWidth(5.0f);
        if (!verts.empty())
        {
            glDrawArrays(GL_LINE_STRIP, 0, verts.size());
        }

        glPointSize(10.0f);
        if (!verts.empty())
        {
            glDrawArrays(GL_POINTS, 0, verts.size());
        }

        if (verts.size() >= 2)
        {
            glUniform1i(dash_location, 1);
            glUniform1f(time_location, time);

            glBindVertexArray(bezier_vao);
            glLineWidth(3.0f);
            int segments = (verts.size() - 1) * quality;
            glDrawArrays(GL_LINE_STRIP, 0, segments + 1);
        }

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
