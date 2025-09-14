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

#include <string>
#include <algorithm>

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


const char fragment_source[] =
R"(#version 330 core

// in vec3 color;
// flat in vec3 color;
in vec2 uv;

layout (location = 0) out vec4 out_color;

void main()
{
    // vec4(R, G, B, A)
    // out_color = vec4(color, 1.0);
    vec2 ij = floor(uv);
    float parity = mod(ij.x + ij.y, 2.0);
    vec3 color = (parity < 1.0) ? vec3(0.0) : vec3(1.0);
    out_color = vec4(color, 1.0);
}
)";

const char vertex_source[] =
R"(#version 330 core

// out vec3 color;
// flat out vec3 color;
out vec2 uv;

const vec2 VERTICES[3] = vec2[3](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0)
);

const vec3 COLORS[3] = vec3[3](
    vec3(0.0, 0.0, 0.0),
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0)
);

void main()
{
    // gl_Position = vec4(VERTICES[gl_VertexID], 0.0, 1.0);
    // color = COLORS[gl_VertexID];
    vec2 p = VERTICES[gl_VertexID];
    gl_Position = vec4(p, 0.0, 1.0);
    uv = p * 20.0;
}
)";

GLuint create_shader(GLenum shader_type, const char * shader_source)
{
    GLuint shader = glCreateShader(shader_type);
    if (shader == 0) throw std::runtime_error("glCreateShader failed");

    glShaderSource(shader, 1, &shader_source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);

        std::string info_log(std::max(1, log_len), '\0');
        GLsizei written = 0;
        glGetShaderInfoLog(shader, log_len, &written, info_log.data());

        glDeleteShader(shader);
        throw std::runtime_error("Shader compile failed:\n" + info_log);
    }

    return shader;
}

GLuint create_program(GLuint vertex_shader, GLuint fragment_shader)
{
    GLuint program = glCreateProgram();
    if (program == 0) throw std::runtime_error("glCreateProgram failed");

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);

    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);

        std::string log(std::max(1, log_len), '\0');
        GLsizei written = 0;
        glGetProgramInfoLog(program, log_len, &written, log.data());

        glDeleteProgram(program);
        throw std::runtime_error("Program link failed:\n" + log);
    }

    return program;
}

int main() try
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        sdl2_fail("SDL_Init: ");

    SDL_Window * window = SDL_CreateWindow("Graphics course practice 1",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        800, 600,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_MAXIMIZED);

    if (!window)
        sdl2_fail("SDL_CreateWindow: ");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
        sdl2_fail("SDL_GL_CreateContext: ");

    if (auto result = glewInit(); result != GLEW_NO_ERROR)
        glew_fail("glewInit: ", result);

    if (!GLEW_VERSION_3_3)
        throw std::runtime_error("OpenGL 3.3 is not supported");


    // glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);

    glClearColor(0.8f, 0.8f, 1.f, 0.f);

    // const char* bad_frag_src = "this is not GLSL";
    // try
    // {
    //     create_shader(GL_FRAGMENT_SHADER, bad_frag_src);
    // }
    // catch (const std::exception& e)
    // {
    //     std::cerr << "Shader compilation error: " << e.what() << std::endl;
    // }

    GLuint vs = create_shader(GL_VERTEX_SHADER,   vertex_source);
    GLuint fs = create_shader(GL_FRAGMENT_SHADER, fragment_source);
    GLuint prog = create_program(vs, fs);

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    bool running = true;
    while (running)
    {
        for (SDL_Event event; SDL_PollEvent(&event);) switch (event.type)
        {
        case SDL_QUIT:
            running = false;
            break;
        }

        if (!running)
            break;

        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(prog);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        SDL_GL_SwapWindow(window);
    }

    glUseProgram(0);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return EXIT_SUCCESS;
}
catch (std::exception const & e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
