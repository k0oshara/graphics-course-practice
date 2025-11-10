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
#include <string>
#include <algorithm>

#define GLM_FORCE_SWIZZLE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#define STB_IMAGE_IMPLEMENTATION
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

// обертка для создания шейдера(программа, которая запускается на видеокарте, и обрабатывает вершины и пиксели)
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

// берет вершинный и фрагментный шейдеры, и превращает в шейдерную программу OpenGL
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

// load texture
GLuint load_texture_2d(const std::string& path)
{
    int w,h,c;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &c, 4);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (!data) {
        uint32_t white = 0xffffffff;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D); // создаем мипмапы (уменьшенные структуры текстур)
        stbi_image_free(data);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return tex;
}

const char scene_vs[] = R"(#version 330 core
layout(location=0) in vec3 in_pos;
layout(location=1) in vec3 in_nor;
layout(location=2) in vec2 in_uv;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vPos;
out vec3 vNor;
out vec2 vUV;

void main() {
    gl_Position = uProj * uView * uModel * vec4(in_pos,1.0);
    vPos = vec3(uModel * vec4(in_pos,1.0));
    mat3 normalMatrix = transpose(inverse(mat3(uModel)));
    vNor = normalize(normalMatrix * in_nor);

    vUV = in_uv;
}
)";

const char scene_fs[] = R"(#version 330 core
in vec3 vPos;
in vec3 vNor;
in vec2 vUV;
out vec4 outColor;

uniform sampler2D uAlbedo;
uniform sampler2D uAlpha;
uniform vec3 uKs;
uniform float uNs;

uniform vec3 uAmbient;

uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform mat4 uSunMVP;
uniform sampler2D uSunShadow;
uniform float uSunBias;

uniform vec3 uPointPos;
uniform vec3 uPointColor;
uniform float uPointRange;
uniform samplerCube uPointShadow;
uniform float uPointFar;
uniform float uPointBias;

float sampleVSM(sampler2D sh, vec4 pos, float bias)
{
    pos /= pos.w;
    pos = pos * 0.5 + 0.5;

    if (pos.x < 0.0 || pos.x > 1.0 || pos.y < 0.0 || pos.y > 1.0 || pos.z < 0.0 || pos.z > 1.0)
        return 1.0;

    vec2 data = texture(sh, pos.xy).rg;
    float mu = data.r;
    float var = max(data.g - mu*mu, 0.00002);
    float z = pos.z - bias;

    if (z <= 0.0) return 1.0;
    if (z <= mu) return 1.0;

    float p = var / (var + (z-mu)*(z-mu));
    const float delta = 0.125;
    return (p <= delta) ? 0.0 : clamp((p - delta)/(1.0 - delta), 0.0, 1.0);
}

// float samplePointVSM(samplerCube sh, vec3 fragPos, vec3 lightPos, float farPlane, float bias)
// {
//     vec3 L = fragPos - lightPos;
//     float dist = length(L);
//     vec3 dir = L / dist;

//     vec2 data = texture(sh, dir).rg;
//     float mu = data.x;
//     float var = max(data.y - mu * mu, 0.00002);
//     float z = dist / farPlane - bias;

//     if (z <= 0.0) return 1.0;
//     if (z <= mu) return 1.0;

//     float p = var / (var + (z - mu)*(z - mu));
//     const float delta = 0.125;
//     return (p <= delta) ? 0.0 : clamp((p - delta)/(1.0 - delta), 0.0, 1.0);
// }

float samplePointShadow(samplerCube sh, vec3 fragPos, vec3 lightPos, float farPlane, float bias)
{
    vec3 L = fragPos - lightPos;
    float distToFrag = length(L);

    float closestDepth = texture(sh, normalize(L)).r * farPlane;

    if (distToFrag - bias > closestDepth) return 0.0;
    else return 1.0;
}
void main()
{
    vec3 albedo = texture(uAlbedo, vUV).rgb;
    float alpha = texture(uAlpha, vUV).r;
    if (alpha < 0.5) discard;

    vec3 N = normalize(vNor);
    vec3 V = normalize(-vPos);

    vec4 sp = uSunMVP * vec4(vPos,1.0);
    float sunShadow = sampleVSM(uSunShadow, sp, uSunBias);

    vec3 Ld = normalize(uSunDir);
    float NdL = max(dot(N,Ld), 0.0);
    vec3 diffSun = albedo * uSunColor * NdL * sunShadow;
    vec3 R = reflect(-Ld, N);
    float specSun = pow(max(dot(R,V),0.0), uNs);
    vec3 specSunCol = uSunColor * uKs * specSun * sunShadow;

    float ptShadow = samplePointShadow(uPointShadow, vPos, uPointPos, uPointFar, uPointBias);

    vec3 Lp = uPointPos - vPos;
    float d = length(Lp);
    Lp /= d;
    float att = clamp(1.0 - d/uPointRange, 0.0, 1.0);
    float NdLp = max(dot(N,Lp),0.0);
    vec3 diffPt = albedo * uPointColor * NdLp * att * ptShadow;
    vec3 Rp = reflect(-Lp, N);
    float specPt = pow(max(dot(Rp, V),0.0), uNs);
    vec3 specPtCol = uPointColor * uKs * specPt * att * ptShadow;

    vec3 color = albedo * uAmbient + diffSun + specSunCol + diffPt + specPtCol;
    outColor = vec4(color,1.0);
}
)";

const char dir_sm_vs[] = R"(#version 330 core
layout(location=0) in vec3 in_pos;
uniform mat4 uModel;
uniform mat4 uLight;

void main()
{
    gl_Position = uLight * uModel * vec4(in_pos,1.0);
}
)";

const char dir_sm_fs[] = R"(#version 330 core
out vec4 outColor;

void main()
{
    float z = gl_FragCoord.z;
    float dx = dFdx(z);
    float dy = dFdy(z);
    float z2 = z*z + 0.25*(dx*dx + dy*dy);
    outColor = vec4(z, z2, 0, 0);
}
)";

const char blur_vs[] = R"(#version 330 core
vec2 q[6] = vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
out vec2 vUV;

void main()
{
    vec2 p = q[gl_VertexID];
    gl_Position = vec4(p,0,1);
    vUV = p*0.5 + 0.5;
}
)";

const char blur_fs[] = R"(#version 330 core
in vec2 vUV;
out vec4 outColor;
uniform sampler2D uTex;
uniform float uTexel;
uniform bool uHorizontal;

void main()
{
    float k[5] = float[](0.06136,0.24477,0.38774,0.24477,0.06136);
    vec2 sum = vec2(0);
    for (int i=-2;i<=2;i++){
        vec2 off = uHorizontal ? vec2(float(i)*uTexel,0.0) : vec2(0.0,float(i)*uTexel);
        vec2 d = texture(uTex, clamp(vUV+off,0.0,1.0)).rg;
        sum += d * k[i+2];
    }
    outColor = vec4(sum,0,0);
}
)";

const char point_sm_vs[] = R"(
#version 330 core
layout(location=0) in vec3 in_pos;

uniform mat4 uModel;
uniform mat4 uShadowMats[6];
uniform int uFace;

out vec3 Pos;

void main()
{
    vec4 wd = uModel * vec4(in_pos, 1.0);
    Pos = wd.xyz;
    gl_Position = uShadowMats[uFace] * wd;
}
)";

const char point_sm_fs[] = R"(#version 330 core
in vec3 Pos;

uniform vec3 uLightPos;
uniform float uFar;

out float outDepth;

void main()
{
    float dist = length(Pos - uLightPos);
    outDepth = dist / uFar;
}
)";

struct GLMaterial {
    GLuint albedo = 0;
    GLuint alpha = 0;
    glm::vec3 Ks{0.0f};
    float Ns = 16.0f;
};

struct Mesh {
    GLuint vao=0, vbo=0, ebo=0;
    GLsizei indexCount=0;
    int matID=-1;
};

int main(int argc, char** argv) try
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

    SDL_Window* window = SDL_CreateWindow("Graphics course homework 2",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280, 720,
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

    std::string project_root = PROJECT_ROOT;
    std::string inputfile = (argc > 1) ? argv[1] : (project_root + "/scenes/sponza/sponza.obj");
    std::string baseDir = inputfile;
    {
        auto pos = baseDir.find_last_of("/\\");
        if (pos != std::string::npos) baseDir = baseDir.substr(0, pos);
        else baseDir = ".";
    }

    tinyobj::ObjReaderConfig cfg;
    cfg.mtl_search_path = baseDir;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(inputfile, cfg))
        throw std::runtime_error("tinyobj: " + reader.Error());

    auto& attrib = reader.GetAttrib();
    auto& shapes = reader.GetShapes();
    auto& materials = reader.GetMaterials();

    auto fixPath = [](std::string s){ for (char& c: s) if (c=='\\') c='/'; return s; };

    std::vector<GLMaterial> glMats(materials.size());
    for (size_t i = 0; i < materials.size(); ++i) {
        const auto& m = materials[i];
        GLMaterial gm;

        // DEBUG
        std::cout << "Material " << i << ": " << m.name << "\n";
        // DEBUG

        std::string albedoName;
        if (!m.ambient_texname.empty()) albedoName = fixPath(m.ambient_texname);
        else if (!m.diffuse_texname.empty()) albedoName = fixPath(m.diffuse_texname);
        if (!albedoName.empty())
        {
            gm.albedo = load_texture_2d(baseDir + "/" + albedoName);
        }
        else
        {
            gm.albedo = load_texture_2d(project_root + "/scenes/sponza/textures/sponza_floor_a_diff.png");

            // DEBUG
            std::cout << "albedoName is empty" << "\n";
            // DEBUG
        }

        std::string alphaName;
        if (!m.alpha_texname.empty()) alphaName = fixPath(m.alpha_texname);
        if (!alphaName.empty())
            gm.alpha = load_texture_2d(baseDir + "/" + alphaName);
        else {
            GLuint t; glGenTextures(1,&t);
            glBindTexture(GL_TEXTURE_2D,t);
            uint32_t white=0xffffffff;
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,&white);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            gm.alpha = t;

            // DEBUG
            std::cout << "alphaName is empty" << "\n";
            // DEBUG
        }

        gm.Ks = glm::vec3(m.specular[0], m.specular[1], m.specular[2]);
        gm.Ns = m.shininess > 0 ? m.shininess : 16.0f;
        glMats[i] = gm;
    }

    std::vector<Mesh> meshes;
    glm::vec3 bboxMin(1e30f), bboxMax(-1e30f);
    for (size_t s = 0; s < shapes.size(); ++s) {
        const auto& shape = shapes[s];
        std::vector<float> verts;
        std::vector<unsigned int> inds;
        int curMat = -1;
        size_t index_offset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            int fv = shape.mesh.num_face_vertices[f];
            for (int v = 0; v < fv; ++v) {
                tinyobj::index_t idx = shape.mesh.indices[index_offset + v];
                float px = attrib.vertices[3*idx.vertex_index+0];
                float py = attrib.vertices[3*idx.vertex_index+1];
                float pz = attrib.vertices[3*idx.vertex_index+2];
                float nx = 0,ny = 0,nz = 0;
                if (idx.normal_index >= 0) {
                    nx = attrib.normals[3*idx.normal_index+0];
                    ny = attrib.normals[3*idx.normal_index+1];
                    nz = attrib.normals[3*idx.normal_index+2];
                }
                float u = 0,vv = 0;
                if (idx.texcoord_index >= 0) {
                    u = attrib.texcoords[2*idx.texcoord_index+0];
                    vv = 1.0f - attrib.texcoords[2*idx.texcoord_index+1];
                }
                bboxMin = glm::min(bboxMin, glm::vec3(px,py,pz));
                bboxMax = glm::max(bboxMax, glm::vec3(px,py,pz));

                verts.push_back(px); verts.push_back(py); verts.push_back(pz);
                verts.push_back(nx); verts.push_back(ny); verts.push_back(nz);
                verts.push_back(u);  verts.push_back(vv);

                inds.push_back((unsigned int)inds.size());
            }
            int mID = shape.mesh.material_ids[f];
            if (mID >= 0) curMat = mID;
            index_offset += fv;
        }

        Mesh m;
        glGenVertexArrays(1,&m.vao);
        glGenBuffers(1,&m.vbo);
        glGenBuffers(1,&m.ebo);
        glBindVertexArray(m.vao);

        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size()*sizeof(unsigned int), inds.data(), GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(6*sizeof(float)));

        m.indexCount = inds.size();
        m.matID = curMat;
        meshes.push_back(m);
    }

    // DEBUG
    std::cout << "shapes: " << shapes.size() << "\n";
    std::cout << "materials: " << materials.size() << "\n";
    std::cout << "bbox min: " << bboxMin.x << " " << bboxMin.y << " " << bboxMin.z << "\n";
    std::cout << "bbox max: " << bboxMax.x << " " << bboxMax.y << " " << bboxMax.z << "\n";
    // DEBUG

    GLuint progScene = create_program(
        create_shader(GL_VERTEX_SHADER, scene_vs),
        create_shader(GL_FRAGMENT_SHADER, scene_fs)
    );
    GLuint progDirSM = create_program(
        create_shader(GL_VERTEX_SHADER, dir_sm_vs),
        create_shader(GL_FRAGMENT_SHADER, dir_sm_fs)
    );
    GLuint progBlur = create_program(
        create_shader(GL_VERTEX_SHADER, blur_vs),
        create_shader(GL_FRAGMENT_SHADER, blur_fs)
    );
    GLuint progPointSM = create_program(
        create_shader(GL_VERTEX_SHADER, point_sm_vs),
        create_shader(GL_FRAGMENT_SHADER, point_sm_fs)
    );

    const int DIR_SM_SIZE = 2048;
    GLuint dirTex;
    glGenTextures(1,&dirTex);
    glBindTexture(GL_TEXTURE_2D, dirTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RG32F,DIR_SM_SIZE,DIR_SM_SIZE,0,GL_RG,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);

    GLuint dirDepth;
    glGenRenderbuffers(1,&dirDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, dirDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, DIR_SM_SIZE, DIR_SM_SIZE);

    GLuint dirFBO;
    glGenFramebuffers(1,&dirFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, dirFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dirTex,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, dirDepth);
    GLenum db[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, db);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        throw std::runtime_error("dir fbo incomplete");

    GLuint tmpTex, blurTex;
    glGenTextures(1,&tmpTex);
    glBindTexture(GL_TEXTURE_2D, tmpTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RG32F,DIR_SM_SIZE,DIR_SM_SIZE,0,GL_RG,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    glGenTextures(1,&blurTex);
    glBindTexture(GL_TEXTURE_2D, blurTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RG32F,DIR_SM_SIZE,DIR_SM_SIZE,0,GL_RG,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    GLuint blurFBO1, blurFBO2;
    glGenFramebuffers(1,&blurFBO1);
    glBindFramebuffer(GL_FRAMEBUFFER, blurFBO1);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmpTex,0);
    glDrawBuffers(1,db);
    glGenFramebuffers(1,&blurFBO2);
    glBindFramebuffer(GL_FRAMEBUFFER, blurFBO2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTex,0);
    glDrawBuffers(1,db);

    const int POINT_SM_SIZE = 2048;
    GLuint pointCube;
    glGenTextures(1,&pointCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, pointCube);
    for (int i=0;i<6;i++)
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+i,0,GL_R32F,POINT_SM_SIZE,POINT_SM_SIZE,0,GL_RED,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);

    GLuint pointDepthRBO;
    glGenRenderbuffers(1, &pointDepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, pointDepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, POINT_SM_SIZE, POINT_SM_SIZE);

    GLuint pointFBO;
    glGenFramebuffers(1,&pointFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, pointFBO);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, pointDepthRBO);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "Point shadow FBO is not complete!\n";
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    auto last_frame_start = std::chrono::high_resolution_clock::now();

    float time = 0.0f;
    bool paused = false;

    float scene_size = glm::length(bboxMax - bboxMin);
    if (scene_size < 1.0f) scene_size = 10.0f;
    glm::vec3 scene_center = (bboxMin + bboxMax) * 0.5f;

    float view_azimuth   = glm::radians(90.0f);
    float view_elevation = glm::radians(-5.0f);
    float camera_distance = scene_size * 0.15f;

    glm::vec3 camera_pos = scene_center + glm::vec3(0.0f, scene_size * 0.05f, 0.0f);

    std::map<SDL_Keycode, bool> button_down;
    bool running = true;
    while (running)
    {
        for (SDL_Event event; SDL_PollEvent(&event); )
        {
            switch (event.type)
            {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED)
                {
                    width = event.window.data1;
                    height = event.window.data2;
                    glViewport(0, 0, width, height);
                }
                break;

            case SDL_KEYDOWN:
                button_down[event.key.keysym.sym] = true;

                if (event.key.keysym.sym == SDLK_ESCAPE)
                    running = false;
                if (event.key.keysym.sym == SDLK_SPACE)
                    paused = !paused;

                break;

            case SDL_KEYUP:
                button_down[event.key.keysym.sym] = false;
                break;
            }
        }

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - last_frame_start).count();
        last_frame_start = now;
        if (!paused)
            time += dt;

        glm::vec3 forward = glm::normalize(glm::vec3(
            cos(view_elevation) * sin(view_azimuth),
            sin(view_elevation),
            cos(view_elevation) * cos(view_azimuth)
        ));
        float move_speed = camera_distance * 2.0f;

        if (button_down[SDLK_w])
            camera_pos += forward * move_speed * dt;
        if (button_down[SDLK_s])
            camera_pos -= forward * move_speed * dt;

        if (button_down[SDLK_LEFT])
            view_azimuth += 2.0f * dt;
        if (button_down[SDLK_RIGHT])
            view_azimuth -= 2.0f * dt;

        if (button_down[SDLK_UP])
            view_elevation += 2.0f * dt;
        if (button_down[SDLK_DOWN])
            view_elevation -= 2.0f * dt;

        const float max_elev = glm::half_pi<float>() - 0.05f;
        const float min_elev = -max_elev;
        if (view_elevation > max_elev)  view_elevation = max_elev;
        if (view_elevation < min_elev)  view_elevation = min_elev;

        glm::vec3 sunDir = glm::normalize(glm::vec3(std::cos(time), 10.0f, std::sin(time)));

        float baseY = scene_size * 0.35f;
        float amp = scene_size * 0.10f;

        glm::vec3 pointPos = scene_center + glm::vec3(
            0.3f,
            baseY + std::sin(time * 1.5f) * amp,
            0.0f
        );

        glm::mat4 view = glm::lookAt(camera_pos, camera_pos + forward, glm::vec3(0,1,0));
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), float(width)/float(height), 0.1f, scene_size * 4.0f);

        glm::vec3 lightZ = -sunDir;
        glm::vec3 lx = glm::normalize(glm::cross(glm::vec3(0,1,0), lightZ));
        glm::vec3 ly = glm::cross(lightZ, lx);
        float hw=0, hh=0, hd=0;
        glm::vec3 corners[8] = {
            {bboxMin.x,bboxMin.y,bboxMin.z}, {bboxMax.x,bboxMin.y,bboxMin.z},
            {bboxMin.x,bboxMax.y,bboxMin.z}, {bboxMax.x,bboxMax.y,bboxMin.z},
            {bboxMin.x,bboxMin.y,bboxMax.z}, {bboxMax.x,bboxMin.y,bboxMax.z},
            {bboxMin.x,bboxMax.y,bboxMax.z}, {bboxMax.x,bboxMax.y,bboxMax.z}
        };
        for (auto& V : corners) {
            glm::vec3 dV = V - scene_center;
            hw = std::max(hw, std::abs(glm::dot(dV,lx)));
            hh = std::max(hh, std::abs(glm::dot(dV,ly)));
            hd = std::max(hd, std::abs(glm::dot(dV,lightZ)));
        }
        glm::vec3 lightPos = scene_center - lightZ * hd;
        glm::mat4 lightV = glm::lookAt(lightPos, scene_center, ly);
        glm::mat4 lightP = glm::ortho(-hw,hw, -hh,hh, 0.0f, 2.0f*hd);
        glm::mat4 lightM = lightP * lightV;

        glViewport(0,0,DIR_SM_SIZE,DIR_SM_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, dirFBO);
        glClearColor(1,1,0,0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(progDirSM);
        for (auto& m : meshes) {
            glm::mat4 model(1.0f);
            glUniformMatrix4fv(glGetUniformLocation(progDirSM,"uModel"),1,GL_FALSE,glm::value_ptr(model));
            glUniformMatrix4fv(glGetUniformLocation(progDirSM,"uLight"),1,GL_FALSE,glm::value_ptr(lightM));
            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
        }

        glDisable(GL_DEPTH_TEST);
        glUseProgram(progBlur);
        glUniform1f(glGetUniformLocation(progBlur,"uTexel"), 1.0f/float(DIR_SM_SIZE));

        glBindFramebuffer(GL_FRAMEBUFFER, blurFBO1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(glGetUniformLocation(progBlur,"uHorizontal"), GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, dirTex);
        glUniform1i(glGetUniformLocation(progBlur,"uTex"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glBindFramebuffer(GL_FRAMEBUFFER, blurFBO2);
        glClear(GL_COLOR_BUFFER_BIT);
        glUniform1i(glGetUniformLocation(progBlur,"uHorizontal"), GL_FALSE);
        glBindTexture(GL_TEXTURE_2D, tmpTex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glEnable(GL_DEPTH_TEST);

        float nearP = 0.1f;
        float farP = scene_size*4.0f;
        glm::mat4 ptProj = glm::perspective(glm::radians(90.0f), 1.0f, nearP, farP);
        glm::mat4 ptMats[6] = {
            ptProj * glm::lookAt(pointPos, pointPos+glm::vec3( 1,0,0), glm::vec3(0,-1,0)),
            ptProj * glm::lookAt(pointPos, pointPos+glm::vec3(-1,0,0), glm::vec3(0,-1,0)),
            ptProj * glm::lookAt(pointPos, pointPos+glm::vec3(0, 1,0), glm::vec3(0,0,1)),
            ptProj * glm::lookAt(pointPos, pointPos+glm::vec3(0,-1,0), glm::vec3(0,0,-1)),
            ptProj * glm::lookAt(pointPos, pointPos+glm::vec3(0,0, 1), glm::vec3(0,-1,0)),
            ptProj * glm::lookAt(pointPos, pointPos+glm::vec3(0,0,-1), glm::vec3(0,-1,0)),
        };

        glViewport(0,0,POINT_SM_SIZE,POINT_SM_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, pointFBO);
        glUseProgram(progPointSM);
        glUniformMatrix4fv(glGetUniformLocation(progPointSM,"uModel"),1,GL_FALSE,glm::value_ptr(glm::mat4(1.0f)));
        glUniformMatrix4fv(glGetUniformLocation(progPointSM,"uShadowMats[0]"),6,GL_FALSE,glm::value_ptr(ptMats[0]));
        glUniform1f(glGetUniformLocation(progPointSM, "uFar"), farP);
        glUniform3fv(glGetUniformLocation(progPointSM, "uLightPos"), 1, glm::value_ptr(pointPos));
        for (int face=0; face<6; ++face) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, pointCube, 0);

            GLenum drawBuf = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &drawBuf);

            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glUniform1i(glGetUniformLocation(progPointSM,"uFace"), face);
            for (auto& m : meshes) {
                glBindVertexArray(m.vao);
                glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0,0,width,height);
        glClearColor(0.05f,0.05f,0.08f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(progScene);
        glUniformMatrix4fv(glGetUniformLocation(progScene,"uView"),1,GL_FALSE,glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(progScene,"uProj"),1,GL_FALSE,glm::value_ptr(proj));
        glUniform3f(glGetUniformLocation(progScene,"uAmbient"), 0.2f,0.2f,0.2f);

        glUniform3fv(glGetUniformLocation(progScene,"uSunDir"),1,glm::value_ptr(sunDir));
        glUniform3f(glGetUniformLocation(progScene,"uSunColor"),0.5f,0.5f,0.5f);
        glUniformMatrix4fv(glGetUniformLocation(progScene,"uSunMVP"),1,GL_FALSE,glm::value_ptr(lightM));
        glUniform1f(glGetUniformLocation(progScene,"uSunBias"), 0.002f);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, blurTex);
        glUniform1i(glGetUniformLocation(progScene,"uSunShadow"), 3);

        glUniform3fv(glGetUniformLocation(progScene,"uPointPos"),1,glm::value_ptr(pointPos));
        glUniform3f(glGetUniformLocation(progScene,"uPointColor"),0.5f,0.5f,0.5f);
        glUniform1f(glGetUniformLocation(progScene,"uPointRange"), scene_size*1.5f);
        glUniform1f(glGetUniformLocation(progScene,"uPointFar"), farP);
        glUniform1f(glGetUniformLocation(progScene,"uPointBias"), 0.01f);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, pointCube);
        glUniform1i(glGetUniformLocation(progScene,"uPointShadow"), 4);

        for (auto& m : meshes) {
            glm::mat4 model(1.0f);
            glUniformMatrix4fv(glGetUniformLocation(progScene,"uModel"),1,GL_FALSE,glm::value_ptr(model));

            GLMaterial gm{};
            if (m.matID >= 0 && m.matID < (int)glMats.size()) gm = glMats[m.matID];

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, gm.albedo);
            glUniform1i(glGetUniformLocation(progScene,"uAlbedo"), 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, gm.alpha);
            glUniform1i(glGetUniformLocation(progScene,"uAlpha"), 1);

            glUniform3fv(glGetUniformLocation(progScene,"uKs"),1,glm::value_ptr(gm.Ks));
            glUniform1f(glGetUniformLocation(progScene,"uNs"), gm.Ns);

            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
        }

        SDL_GL_SwapWindow(window);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
catch (std::exception const& e)
{
    std::cerr << e.what() << "\n";
    return 1;
}
