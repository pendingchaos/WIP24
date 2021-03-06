#define DEFAULTS "*delay:          33333\n"\
                 "*showFPS:        False\n"

#define API_KEY "ftntwN"

#include <stdint.h>
#include <stdbool.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <pwd.h>
#include <stdarg.h>
#include <errno.h>

#include "stb_image.h"
#include "xlockmore.h"
#include "texfont.h"
#include "json.h"

#ifdef USE_GL
typedef enum {channel_none, channel_image} wip24_channel_type;

typedef struct {
    wip24_channel_type type;
    GLuint texture;
} wip24_channel;

typedef struct {
    GLXContext *glx_context;
    GLuint program;
    wip24_channel channels[4];
    uint64_t start_time;
    uint64_t time_delta;
    unsigned int frame_count;
    float undersample;
    float undersamples[4];
    GLuint framebuffer;
    GLuint fb_texture;
    texture_font_data* font;
    char shader_info[1024];
} wip24_state;

static const char* source_header = "uniform vec3 iResolution;\n"
                                   "uniform float iGlobalTime;\n"
                                   "uniform float iChannelTime[4];\n"
                                   "uniform vec4 iMouse;\n"
                                   "uniform vec4 iDate;\n"
                                   "uniform float iSampleRate;\n"
                                   "uniform vec3 iChannelResolution[4];\n"
                                   "uniform int iFrame;\n"
                                   "uniform float iTimeDelta;\n"
                                   "struct Channel {vec3 resolution; float time;};\n"
                                   "uniform Channel iChannel[4];\n"
                                   "uniform sampler2D iChannel0;\n"
                                   "uniform sampler2D iChannel1;\n"
                                   "uniform sampler2D iChannel2;\n"
                                   "uniform sampler2D iChannel3;\n"
                                   "void mainImage(out vec4 fragColor, in vec2 fragCoord);\n"
                                   "#define texture2DLodEXT(sampler, p, lod) texture2D(sampler, p)\n"
                                   "#define texture2DGradEXT(sampler, p, dPdx, dPdy) texture2D(sampler, p)\n"
                                   "#define iTime iGlobalTime\n"
                                   "void main() {\n"
                                   "    gl_FragColor = vec4(vec3(0.0), 1.0);\n"
                                   "    mainImage(gl_FragColor, gl_FragCoord.xy);\n"
                                   "    gl_FragColor.a = 1.0;\n"
                                   "}\n"
                                   "#line 1\n";
static wip24_state *states = NULL;
static float undersample_max = 16.0f;
static float shader_duration = 300.0f;
static XrmOptionDescRec opts[] = {{"-undersampleMax", ".undersampleMax", XrmoptionSepArg, NULL},
                                  {"-shaderDuration", ".shaderDuration", XrmoptionSepArg, NULL}};
static argtype vars[] = {{&undersample_max, "undersampleMax", "Undersample Maximum", "16.0", t_Float},
                         {&shader_duration, "shaderDuration", "Shader Duration", "300.0", t_Float}};

ENTRYPOINT ModeSpecOpt wip24_opts = {sizeof(opts)/sizeof(XrmOptionDescRec), opts,
                                     sizeof(vars)/sizeof(argtype), vars,
                                     NULL};

static uint64_t get_time() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec*(uint64_t)1000000000 + t.tv_nsec;
}

static const char* get_home_dir() {
    char* res = getenv("HOME");
    if (!res) res = getpwuid(getuid())->pw_dir;
    return res;
}

static void ensure_cache_dir() {
    char dir[4096];
    strcpy(dir, get_home_dir());
    strcat(dir, "/.wip24");
    mkdir(dir, S_IRWXU);
    strcat(dir, "/cache");
    mkdir(dir, S_IRWXU);
    strcat(dir, "/images");
    mkdir(dir, S_IRWXU);
    
    strcpy(dir, get_home_dir());
    strcat(dir, "/.wip24/cache/shaders");
    mkdir(dir, S_IRWXU);
}

static void log_entry(const char*format, ...) {
    va_list list;
    va_list list2;
    va_start(list, format);
    va_copy(list2, list);
    
    char filename[4096];
    snprintf(filename, sizeof(filename), "%s/.wip24/log.txt", get_home_dir());
    
    FILE* file = fopen(filename, "a");
    while (lockf(fileno(file), F_TLOCK, 0)<0 && errno==EAGAIN);
    
    fprintf(file, "[PID %lld] ", (long long)getpid());
    vfprintf(file, format, list);
    
    lockf(fileno(file), F_ULOCK, 0);
    fclose(file);
    
    va_end(list);
    vprintf(format, list2);
}

typedef struct {
    size_t data_size;
    char* data;
} write_callback_data;

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    write_callback_data* data = userdata;
    
    data->data = realloc(data->data, data->data_size+size*nmemb);
    
    memcpy(data->data+data->data_size, ptr, size*nmemb);
    data->data_size += size * nmemb;
    
    return size * nmemb;
}

static void read_data(const char* base_url, void** data, size_t* size) {
    CURL* handle = curl_easy_init();
    
    char* url = calloc(1, strlen(base_url)+9);
    strcat(url, "https://");
    strcat(url, base_url);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    log_entry("Reading from %s\n", url);
    free(url);
    
    write_callback_data cb_data;
    cb_data.data_size = 0;
    cb_data.data = NULL;
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &cb_data);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &write_callback);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, (long)1);
    
    CURLcode res = curl_easy_perform(handle);
    curl_easy_cleanup(handle);
    if (res != CURLE_OK) {
        log_entry("Error while reading: %s\n", curl_easy_strerror(res));
        *data = NULL;
        *size = 0;
    } else {
        *data = cb_data.data;
        *size = cb_data.data_size;
    }
}

static const char* pick_shader_id() {
    char filename[4096];
    snprintf(filename, sizeof(filename), "%s/.wip24/shaders.txt", get_home_dir());
    FILE* file = fopen(filename, "r");
    if (!file) return NULL;
    
    static char ids[4096][8];
    size_t id_count = 0;
    while (id_count<4096) {
        char id[8] = {0};
        int c;
        while ((c=fgetc(file)) != '\n') {
            if (c == EOF) goto end;
            if (strlen(id)<7) id[strlen(id)] = c;
        }
        
        if (id[0] != '#') strcpy(ids[id_count++], id);
    }
    end:
        ;
    fclose(file);
    
    if (!id_count) return NULL;
    
    return ids[ya_random()%id_count];
}

static void load_texture(wip24_channel* channel, const char* src,
                         const char* filter, const char* wrap,
                         const char* vflip, const char* srgb) {
    glDeleteTextures(1, &channel->texture);
    
    char cached_file[4096] = {0};
    snprintf(cached_file, sizeof(cached_file), "%s/.wip24/cache/images/", get_home_dir());
    for (const char* c = src; *c && strlen(cached_file)+1<sizeof(cached_file); c++)
        cached_file[strlen(cached_file)] = *c=='/' ? '_' : *c;
    FILE* cached = fopen(cached_file, "rb");
    if (!cached) {
        char url[2048];
        snprintf(url, sizeof(url), "shadertoy.com%s", src);
        void* img_data;
        size_t img_data_size;
        read_data(url, &img_data, &img_data_size);
        if (!img_data) goto error;
        
        cached = fopen(cached_file, "wb");
        fwrite(img_data, img_data_size, 1, cached);
        free(img_data);
    }
    fclose(cached);
    
    int w, h, comp;
    stbi_uc* data = stbi_load(cached_file, &w, &h, &comp, 4);
    if (!data) {
        log_entry("Unable to load %s: %s\n", cached_file, stbi_failure_reason());
        goto error;
    }
    
    if (!strcmp(vflip, "true")) {
        for (unsigned int y = 0; y < h; y++) {
            for (unsigned int x = 0; x < w; x++) {
                int temp = ((int*)data)[y*w+x];
                ((int*)data)[y*w+x] = ((int*)data)[(h-y-1)*w+x];
                ((int*)data)[(h-y-1)*w+x] = temp;
            }
        }
    }
    
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    
    if (!strcmp(filter, "mipmap")) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else if (!strcmp(filter, "linear")) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, strcmp(wrap, "repeat")?GL_CLAMP_TO_EDGE:GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, strcmp(wrap, "repeat")?GL_CLAMP_TO_EDGE:GL_REPEAT);
    
    glTexImage2D(GL_TEXTURE_2D, 0, strcmp(srgb, "true")?GL_RGBA:GL_SRGB8_ALPHA8,
                 w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    
    channel->texture = texture;
    channel->type = channel_image;
    return;
    error:
        channel->texture = 0;
        channel->type = channel_none;
}

static void clear_shader(wip24_state* state) {
    glDeleteProgram(state->program);
    state->program = 0;
    for (unsigned int i = 0; i < 4; i++) {
        glDeleteTextures(1, &state->channels[i].texture);
        state->channels[i].type = channel_none;
    }
}

static void set_shader_source(wip24_state* state, const char* source) {
    GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
    
    const char* sources[2] = {source_header, source};
    glShaderSource(frag, 2, sources, NULL);
    glCompileShader(frag);
    GLint status;
    glGetShaderiv(frag, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetShaderInfoLog(frag, sizeof(log), NULL, log);
        log_entry("Error: Unable to compile shader: %s\n", log);
        glDeleteShader(frag);
        return;
    }
    
    state->program = glCreateProgram();
    glAttachShader(state->program, frag);
    glLinkProgram(state->program);
    glValidateProgram(state->program);
    glDeleteShader(frag);
    glGetProgramiv(state->program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[1024];
        glGetProgramInfoLog(state->program, sizeof(log), NULL, log);
        log_entry("Error: Unable to link program: %s\n", log);
        glDeleteProgram(state->program);
        state->program = 0;
        return;
    }
}

static json_value* lookup_obj(json_value* obj, const char* key) {
    if (!obj) return NULL;
    if (obj->type != json_object) return NULL;
    
    for (unsigned int i = 0; i < obj->u.object.length; i++)
        if (!strcmp(obj->u.object.values[i].name, key))
            return obj->u.object.values[i].value;
    
    return NULL;
}

static bool set_shader_from_json(wip24_state* state, const char* json, size_t json_len) {
    glDeleteProgram(state->program);
    state->program = 0;
    
    char error[json_error_max];
    memset(error, 0, json_error_max);
    json_settings settings;
    memset(&settings, 0, sizeof(settings));
    json_value* root = json_parse_ex(&settings, json, json_len, error);
    if (!root) {
        log_entry("Unable to parse JSON: %s\n", error);
        goto error;
    }
    
    json_value* error_val = lookup_obj(root, "Error");
    if (error_val && error_val->type==json_string) {
        log_entry("Error from shadertoy.com: %s\n", error_val->u.string.ptr);
        goto error;
    }
    
    json_value* shader = lookup_obj(root, "Shader");
    
    json_value* renderpasses = lookup_obj(shader, "renderpass");
    if (!renderpasses) goto error;
    if (renderpasses->type != json_array) goto error;
    json_value* renderpass = NULL;
    for (unsigned int i = 0; (i<renderpasses->u.array.length)&&!renderpass; i++) {
        json_value* type = lookup_obj(renderpasses->u.array.values[i], "type");
        if (type && type->type==json_string && !strcmp(type->u.string.ptr, "image"))
            renderpass = renderpasses->u.array.values[i];
    }
    
    json_value* code = lookup_obj(renderpass, "code");
    json_value* inputs = lookup_obj(renderpass, "inputs");
    if (!code || !inputs) goto error;
    if (code->type!=json_string || inputs->type!=json_array) goto error;
    set_shader_source(state, code->u.string.ptr);
    
    for (unsigned int i = 0; i < inputs->u.array.length; i++) {
        json_value* input = inputs->u.array.values[i];
        json_value* src = lookup_obj(input, "src");
        json_value* ctype = lookup_obj(input, "type");
        json_value* channel = lookup_obj(input, "channel");
        json_value* sampler = lookup_obj(input, "sampler");
        json_value* filter = lookup_obj(sampler, "filter");
        json_value* wrap = lookup_obj(sampler, "wrap");
        json_value* vflip = lookup_obj(sampler, "vflip");
        json_value* srgb = lookup_obj(sampler, "srgb");
        if (!src || !ctype || !channel || !sampler || !filter || !wrap ||
            !vflip || !srgb) goto error;
        if (src->type!=json_string || ctype->type!=json_string || srgb->type!=json_string ||
            filter->type!=json_string || wrap->type!=json_string || vflip->type!=json_string ||
            sampler->type!=json_object) goto error;
        if (channel->type != json_integer && channel->type != json_double) goto error;
        if (strcmp(ctype->u.string.ptr, "texture")) goto unsupported;
        json_int_t channel_idx = channel->type==json_integer?channel->u.integer:channel->u.dbl;
        if (channel_idx<0 || channel_idx>3) goto error;
        load_texture(state->channels+channel_idx, src->u.string.ptr, filter->u.string.ptr,
                     wrap->u.string.ptr, vflip->u.string.ptr, srgb->u.string.ptr);
    }
    
    json_value* name = lookup_obj(lookup_obj(shader, "info"), "name");
    json_value* author = lookup_obj(lookup_obj(shader, "info"), "username");
    if (!name || !author) goto error;
    if (name->type!=json_string || author->type!=json_string) goto error;
    snprintf(state->shader_info, sizeof(state->shader_info), "%s by %s",
             name->u.string.ptr, author->u.string.ptr);
    
    json_value_free(root);
    return true;
    error:
        if (root) json_value_free(root);
        clear_shader(state);
        log_entry("Unable to interpret JSON.\n");
        return false;
    unsupported:
        if (root) json_value_free(root);
        clear_shader(state);
        log_entry("Shader uses an unsupported feature.\n");
        return false;
}

static bool set_shader_from_id(wip24_state* state, const char* id) {
    log_entry("Setting shader to %s\n", id);
    
    char cached_file[4096];
    snprintf(cached_file, sizeof(cached_file), "%s/.wip24/cache/shaders/%s.json", get_home_dir(), id);
    FILE* cached = fopen(cached_file, "r");
    if (!cached) {
        char url[2048];
        snprintf(url, sizeof(url), "www.shadertoy.com/api/v1/shaders/%s?key=%s", id, API_KEY);
        
        char* json;
        size_t json_len;
        read_data(url, (void**)&json, &json_len);
        if (!json) return false;
        if (!set_shader_from_json(state, json, json_len)) return false;
        
        if (state->program) {
            cached = fopen(cached_file, "w");
            fwrite(json, json_len, 1, cached);
            fclose(cached);
        }
        
        free(json);
    } else {
        fseek(cached, 0, SEEK_END);
        size_t size = ftell(cached);
        fseek(cached, 0, SEEK_SET);
        
        char *json = (char *)malloc(size);
        memset(json, 0, size);
        fread(json, 1, size, cached);
        
        if (!set_shader_from_json(state, json, size)) return false;
        free(json);
        
        fclose(cached);
        
        struct stat buf;
        stat(cached_file, &buf);
        
        if (difftime(time(NULL), buf.st_mtime) > 172800) { //Two days
            remove(cached_file);
            log_entry("Removing cached shader %s\n", id);
        }
    }
    return true;
}

ENTRYPOINT void reshape_wip24(ModeInfo *mi, int width, int height) {
    wip24_state* state = states + MI_SCREEN(mi);
    glXMakeCurrent(MI_DISPLAY(mi), MI_WINDOW(mi), *(state->glx_context));
    glViewport(0, 0, width, height);
    
    glBindTexture(GL_TEXTURE_2D, state->fb_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width/state->undersample,
                 height/state->undersample, 0, GL_RGB, GL_BYTE, NULL);
}

ENTRYPOINT void refresh_wip24(ModeInfo *mi) {}

ENTRYPOINT void release_wip24(ModeInfo *mi) {
    wip24_state* state = states + MI_SCREEN(mi);
    glXMakeCurrent(MI_DISPLAY(mi), MI_WINDOW(mi), *(state->glx_context));
    
    for (unsigned int i = 0; i < 4; i++)
        glDeleteTextures(1, &state->channels[i].texture);
    glDeleteFramebuffers(1, &state->framebuffer);
    glDeleteTextures(1, &state->fb_texture);
    glDeleteProgram(state->program);
    free_texture_font(state->font);
    glXDestroyContext(MI_DISPLAY(mi), *state->glx_context);
    free(state->glx_context);
}

static void cleanup() {
    curl_global_cleanup();
    free(states);
}

ENTRYPOINT Bool wip24_handle_event(ModeInfo *mi, XEvent *event) {
    return False; /*The event was not handled*/
}

static void gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity,
                              GLsizei length, const char *message, const void *user_param) {
    log_entry("OpenGL debug callback: %s\n", message);
}

static void init_shader(ModeInfo* mi, wip24_state* state) {
    clear_shader(state);
    
    state->start_time = get_time();
    state->time_delta = 0;
    state->frame_count = 0;
    state->undersample = state->undersamples[0] = state->undersamples[1] = 
    state->undersamples[2] = state->undersamples[3] = 1.0f;
    
    reshape_wip24(mi, MI_WIDTH(mi), MI_HEIGHT(mi));
    
    for (size_t i = 0; i < 16; i++) {
        const char* id = pick_shader_id();
        if (!id) {
            log_entry("Unable to pick a shader\n");
            return;
        }
        if (set_shader_from_id(state, id)) return; 
    }
    log_entry("Unable to set a shader\n");
}

ENTRYPOINT void init_wip24(ModeInfo *mi) {
    if (!states) {
        atexit(&cleanup);
        states = calloc(1, MI_NUM_SCREENS(mi)*sizeof(wip24_state));
        curl_global_init(CURL_GLOBAL_DEFAULT);
        log_entry("New process\n");
        ensure_cache_dir();
    }
    
    log_entry("Begin initialization for screen %d\n", MI_SCREEN(mi));
    
    wip24_state* state = states + MI_SCREEN(mi);
    
    state->glx_context = init_GL(mi);
    state->program = 0;
    state->font = load_texture_font(MI_DISPLAY(mi), "fpsFont");
    
    glXMakeCurrent(MI_DISPLAY(mi), MI_WINDOW(mi), *(state->glx_context));
    if (strstr((const char*)glGetString(GL_EXTENSIONS), "GL_ARB_debug_output")) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallbackARB((GLDEBUGPROCARB)gl_debug_callback, NULL);
    }
    glGenFramebuffers(1, &state->framebuffer);
    glGenTextures(1, &state->fb_texture);
    
    glBindFramebuffer(GL_FRAMEBUFFER, state->framebuffer);
    glBindTexture(GL_TEXTURE_2D, state->fb_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, state->fb_texture, 0);
    glEnable(GL_TEXTURE_2D);
    
    init_shader(mi, state);
    
    log_entry("End initialization for screen %d\n", MI_SCREEN(mi));
}

static void update_uniforms(ModeInfo* mi, wip24_state* state) {
    GLint loc = glGetUniformLocation(state->program, "iResolution");
    glUniform3f(loc, MI_WIDTH(mi)/state->undersample, MI_HEIGHT(mi)/state->undersample, 1.0f);
    
    loc = glGetUniformLocation(state->program, "iGlobalTime");
    glUniform1f(loc, (get_time()-state->start_time) / 1000000000.0f);
    
    loc = glGetUniformLocation(state->program, "iTimeDelta");
    glUniform1f(loc, state->time_delta);
    
    loc = glGetUniformLocation(state->program, "iFrame");
    glUniform1i(loc, state->frame_count);
    
    loc = glGetUniformLocation(state->program, "iMouse");
    glUniform4f(loc, 0.0f, (MI_HEIGHT(mi)-1)/state->undersample, 0.0f, 0.0f);
    
    static const char* channels[] = {"iChannel0", "iChannel1", "iChannel2", "iChannel3"};
    static const char* channelsRes1[] = {"iChannel[0].resolution", "iChannel[1].resolution",
                                         "iChannel[2].resolution", "iChannel[3].resolution"};
    static const char* channelsTime1[] = {"iChannel[0].time", "iChannel[1].time",
                                          "iChannel[2].time", "iChannel[3].time"};
    static const char* channelsRes2[] = {"iChannelResolution[0]", "iChannelResolution[1]",
                                         "iChannelResolution[2]", "iChannelResolution[3]"};
    static const char* channelsTime2[] = {"iChannelTime[0]", "iChannelTime[1]",
                                          "iChannelTime[2]", "iChannelTime[3]"};
    for (unsigned int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0+i);
        glBindTexture(GL_TEXTURE_2D, state->channels[i].texture);
        loc = glGetUniformLocation(state->program, channels[i]);
        glUniform1i(loc, i);
        if (!state->channels[i].texture) continue;
        GLint w, h;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        loc = glGetUniformLocation(state->program, channelsRes1[i]);
        glUniform3f(loc, w, h, 1.0);
        loc = glGetUniformLocation(state->program, channelsRes2[i]);
        glUniform3f(loc, w, h, 1.0);
        loc = glGetUniformLocation(state->program, channelsTime1[i]);
        glUniform1f(loc, 0.0f);
        loc = glGetUniformLocation(state->program, channelsTime2[i]);
        glUniform1f(loc, 0.0f);
    }
    glActiveTexture(GL_TEXTURE0);
    
    time_t tm = time(NULL);
    struct tm* date = localtime(&tm);
    loc = glGetUniformLocation(state->program, "iDate");
    glUniform4f(loc, date->tm_year+1900, date->tm_mon, date->tm_mday,
                date->tm_sec);
}

ENTRYPOINT void draw_wip24(ModeInfo *mi) {
    uint64_t frame_start = get_time();
    wip24_state* state = states + MI_SCREEN(mi);
    
    glXMakeCurrent(MI_DISPLAY(mi), MI_WINDOW(mi), *(state->glx_context));
    
    glBindFramebuffer(GL_FRAMEBUFFER, state->framebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, MI_WIDTH(mi)/state->undersample, MI_HEIGHT(mi)/state->undersample);
    if (state->program) {
        glUseProgram(state->program);
        update_uniforms(mi, state);
        glRectf(-1.0f, -1.0f, 1.0f, 1.0f);
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, MI_WIDTH(mi), MI_HEIGHT(mi));
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, state->fb_texture);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(1.0f, 1.0f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(-1.0f, 1.0f);
    glEnd();
    
    if (mi->fps_p) do_fps(mi);
    
    print_texture_label(MI_DISPLAY(mi), state->font, MI_WIDTH(mi), MI_HEIGHT(mi),
                        get_boolean_resource(MI_DISPLAY(mi), "fpsTop", "FPSTop")?2:1,
                        state->shader_info);
    
    glXSwapBuffers(MI_DISPLAY(mi), MI_WINDOW(mi));
    
    state->time_delta = get_time() - frame_start;
    state->frame_count++;
    
    float dest = mi->pause / 1000.0f;
    float current = state->time_delta / 1000000.0f;
    
    float* undersample = state->undersamples + state->frame_count%4;
    if (current > (dest+1.0f)) *undersample += 0.1f;
    else if (current<(dest-1.0f) && state->undersample>1.0f) *undersample -= 0.1f;
    else goto no_reshape;
    
    state->undersample = (state->undersamples[0]+state->undersamples[1]+
                          state->undersamples[2]+state->undersamples[3]) / 2.0f;
    state->undersample = state->undersample>undersample_max ? undersample_max:state->undersample;
    reshape_wip24(mi, MI_WIDTH(mi), MI_HEIGHT(mi));
    no_reshape:
        ;
    
    mi->pause = dest>current ? dest-current : 0;
    
    if ((get_time() - state->start_time)/1000000000.0f > shader_duration)
        init_shader(mi, state);
}

XSCREENSAVER_MODULE("WIP24", wip24)
#endif
