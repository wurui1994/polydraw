/* port/a64/winshim/gl/gl.h —— 把 `#include <gl/gl.h>` 接到本机的 OpenGL 头上。
 *
 * Windows 那份 `gl/gl.h` 只有 GL 1.1；GL 2.0 的那一批（glUniform* / glCreateShader …）
 * 在 polydraw 里是**自己一张函数指针表**（`pd/pd_head.h`，运行期 wglGetProcAddress 填）。
 * 而 macOS 的 `OpenGL/gl.h` 把它们当真函数声明了 —— 43 个 redefinition。
 *
 * 这儿的办法：先把 SDK 那份包进来（真函数照旧叫原名、链接时从 framework 取），
 * **然后把这 43 个名字改成 `pd_*`**。宏是从这一行往后生效的，所以 `pd_head.h` 里
 * 那张指针表与后面全部调用点会一致地变成 `pd_glUniform1f` 这种名字，自己一套，
 * 与 SDK 那套不打架。原文一个字节都不用改。
 *
 * `GL_GLEXT_LEGACY` 顺带开着（SDK gl.h 第 43 行那个开关），少拉一堆 glext。
 */
#ifndef PD_WINSHIM_GL_H
#define PD_WINSHIM_GL_H
#define GL_SILENCE_DEPRECATION 1
#define GL_GLEXT_LEGACY 1
#include <OpenGL/gl.h>

/* GL 2.0 那一批：让位给 pd_head.h 自己的指针表。 */
#define glActiveTexture pd_glActiveTexture
#define glAttachShader pd_glAttachShader
#define glBeginQuery pd_glBeginQuery
#define glCompileShader pd_glCompileShader
#define glCreateProgram pd_glCreateProgram
#define glCreateShader pd_glCreateShader
#define glDeleteProgram pd_glDeleteProgram
#define glDeleteQueries pd_glDeleteQueries
#define glDeleteShader pd_glDeleteShader
#define glDetachShader pd_glDetachShader
#define glEndQuery pd_glEndQuery
#define glGenQueries pd_glGenQueries
#define glGetAttribLocation pd_glGetAttribLocation
#define glGetProgramiv pd_glGetProgramiv
#define glGetQueryObjectiv pd_glGetQueryObjectiv
#define glGetQueryObjectuiv pd_glGetQueryObjectuiv
#define glGetShaderiv pd_glGetShaderiv
#define glGetUniformLocation pd_glGetUniformLocation
#define glLinkProgram pd_glLinkProgram
#define glShaderSource pd_glShaderSource
#define glTexImage3D pd_glTexImage3D
#define glTexSubImage3D pd_glTexSubImage3D
#define glUniform1f pd_glUniform1f
#define glUniform1fv pd_glUniform1fv
#define glUniform1i pd_glUniform1i
#define glUniform1iv pd_glUniform1iv
#define glUniform2f pd_glUniform2f
#define glUniform2fv pd_glUniform2fv
#define glUniform2i pd_glUniform2i
#define glUniform2iv pd_glUniform2iv
#define glUniform3f pd_glUniform3f
#define glUniform3fv pd_glUniform3fv
#define glUniform3i pd_glUniform3i
#define glUniform3iv pd_glUniform3iv
#define glUniform4f pd_glUniform4f
#define glUniform4fv pd_glUniform4fv
#define glUniform4i pd_glUniform4i
#define glUniform4iv pd_glUniform4iv
#define glUseProgram pd_glUseProgram
#define glVertexAttrib1f pd_glVertexAttrib1f
#define glVertexAttrib2f pd_glVertexAttrib2f
#define glVertexAttrib3f pd_glVertexAttrib3f
#define glVertexAttrib4f pd_glVertexAttrib4f

/* `GLhandleARB`：SDK 说是 `void *`，pd_head.h:111 说是 `unsigned int`（Windows 的口径）。
   一样让位 —— 这个类型只在 pd 自己那套里用。 */
#define GLhandleARB pd_GLhandleARB

#endif
