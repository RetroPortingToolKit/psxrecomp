/* Private presentation target and immutable GPU texture versions. This uses
 * ordinary PS1 material shaders, with independent programs/VAOs and owned
 * surfaces. No native render transaction, replay scope or temporal image cache. */
#define PGPU_VERSIONS 16
typedef struct PgpuVersion {
    uint64_t id, epoch;
    GLuint tex, fbo;
    uint32_t references;
} PgpuVersion;
static PgpuVersion s_pgpu_versions[PGPU_VERSIONS];
static uint64_t s_pgpu_next_id = 1;
static GLuint s_pgpu_tex_prog, s_pgpu_geo_prog, s_pgpu_tex_vao, s_pgpu_geo_vao;
static GLuint s_pgpu_tex_vbo, s_pgpu_geo_vbo, s_pgpu_color, s_pgpu_fbo, s_pgpu_stencil;
static int s_pgpu_output,s_pgpu_ready,s_pgpu_init_failed;
static GLint s_pgpu_maskset,s_pgpu_semimode,s_pgpu_semipass;

typedef void (APIENTRY *PgpuStencilFuncSeparate)(GLenum,GLenum,GLint,GLuint);
typedef void (APIENTRY *PgpuStencilMaskSeparate)(GLenum,GLuint);
typedef void (APIENTRY *PgpuStencilOpSeparate)(GLenum,GLenum,GLenum,GLenum);
static PgpuStencilFuncSeparate pgpu_stencil_func;
static PgpuStencilMaskSeparate pgpu_stencil_mask;
static PgpuStencilOpSeparate pgpu_stencil_op;

typedef struct PgpuState {
    GLint draw_fbo, read_fbo, program, vao, buffer, active, textures[3];
    GLint viewport[4], scissor[4], src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
    GLint stencil[2][7], stencil_clear, pack_buffer, pack_row, pack_alignment, unpack_buffer, renderbuffer;
    GLfloat blend_color[4], clear_color[4];
    GLboolean color_mask[4], depth_mask, enables[7];
} PgpuState;
static const GLenum pgpu_enables[7] = {
    GL_BLEND,GL_STENCIL_TEST,GL_DEPTH_TEST,GL_SCISSOR_TEST,GL_CULL_FACE,
    0x8DB9, /* FRAMEBUFFER_SRGB */ 0x8C89 /* RASTERIZER_DISCARD */
};
static void pgpu_state_save(PgpuState *s) {
    glGetIntegerv(0x8CA6,&s->draw_fbo);glGetIntegerv(0x8CAA,&s->read_fbo);
    glGetIntegerv(0x8B8D,&s->program);glGetIntegerv(0x85B5,&s->vao);
    glGetIntegerv(0x8894,&s->buffer);glGetIntegerv(0x84E0,&s->active);
    for(unsigned i=0;i<3;++i) {
        p_glActiveTexture(PSXGL_TEXTURE0+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&s->textures[i]);
    }
    p_glActiveTexture((GLenum)s->active);
    glGetIntegerv(GL_VIEWPORT,s->viewport);glGetIntegerv(GL_SCISSOR_BOX,s->scissor);
    glGetIntegerv(0x80C9,&s->src_rgb);glGetIntegerv(0x80C8,&s->dst_rgb);
    glGetIntegerv(0x80CB,&s->src_a);glGetIntegerv(0x80CA,&s->dst_a);
    glGetIntegerv(0x8009,&s->eq_rgb);glGetIntegerv(0x883D,&s->eq_a);
    glGetFloatv(0x8005,s->blend_color);glGetFloatv(GL_COLOR_CLEAR_VALUE,s->clear_color);
    glGetBooleanv(GL_COLOR_WRITEMASK,s->color_mask);glGetBooleanv(GL_DEPTH_WRITEMASK,&s->depth_mask);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE,&s->stencil_clear);
    const GLenum front[7]={GL_STENCIL_FUNC,GL_STENCIL_REF,GL_STENCIL_VALUE_MASK,
        GL_STENCIL_WRITEMASK,GL_STENCIL_FAIL,GL_STENCIL_PASS_DEPTH_FAIL,GL_STENCIL_PASS_DEPTH_PASS};
    const GLenum back[7]={0x8800,0x8CA3,0x8CA4,0x8CA5,0x8801,0x8802,0x8803};
    for(unsigned i=0;i<7;++i) {
        glGetIntegerv(front[i],&s->stencil[0][i]);glGetIntegerv(back[i],&s->stencil[1][i]);
        s->enables[i]=glIsEnabled(pgpu_enables[i]);
    }
    glGetIntegerv(0x88ED,&s->pack_buffer);glGetIntegerv(PSXGL_PACK_ROW_LENGTH,&s->pack_row);
    glGetIntegerv(GL_PACK_ALIGNMENT,&s->pack_alignment);
    glGetIntegerv(0x88EF,&s->unpack_buffer);glGetIntegerv(0x8CA7,&s->renderbuffer);
}
static void pgpu_state_restore(const PgpuState *s) {
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,(GLuint)s->draw_fbo);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,(GLuint)s->read_fbo);
    p_glUseProgram((GLuint)s->program);p_glBindVertexArray((GLuint)s->vao);
    p_glBindBuffer(PSXGL_ARRAY_BUFFER,(GLuint)s->buffer);
    for(unsigned i=0;i<3;++i) {
        p_glActiveTexture(PSXGL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,(GLuint)s->textures[i]);
    }
    p_glActiveTexture((GLenum)s->active);
    glViewport(s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3]);
    glScissor(s->scissor[0],s->scissor[1],s->scissor[2],s->scissor[3]);
    p_glBlendFuncSeparate(s->src_rgb,s->dst_rgb,s->src_a,s->dst_a);
    p_glBlendEquationSeparate(s->eq_rgb,s->eq_a);
    p_glBlendColor(s->blend_color[0],s->blend_color[1],s->blend_color[2],s->blend_color[3]);
    glClearColor(s->clear_color[0],s->clear_color[1],s->clear_color[2],s->clear_color[3]);
    glClearStencil(s->stencil_clear);glDepthMask(s->depth_mask);
    glColorMask(s->color_mask[0],s->color_mask[1],s->color_mask[2],s->color_mask[3]);
    for(unsigned i=0;i<2;++i) {
        const GLint *v=s->stencil[i];GLenum face=i?GL_BACK:GL_FRONT;
        pgpu_stencil_func(face,v[0],v[1],(GLuint)v[2]);pgpu_stencil_mask(face,(GLuint)v[3]);
        pgpu_stencil_op(face,v[4],v[5],v[6]);
    }
    for(unsigned i=0;i<7;++i) {
        if(s->enables[i])glEnable(pgpu_enables[i]);else glDisable(pgpu_enables[i]);
    }
    p_glBindBuffer(0x88EB,(GLuint)s->pack_buffer);
    p_glBindBuffer(0x88EC,(GLuint)s->unpack_buffer);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER,(GLuint)s->renderbuffer);
    glPixelStorei(PSXGL_PACK_ROW_LENGTH,s->pack_row);glPixelStorei(GL_PACK_ALIGNMENT,s->pack_alignment);
}
static int pgpu_available(void) {
    if(!s_ctx || !s_raster_ok || gr_backend()!=GR_BACKEND_OPENGL || s_depth24_skip_up || s_pass_active)return 0;
    if(!pgpu_stencil_func) {
        pgpu_stencil_func=(PgpuStencilFuncSeparate)SDL_GL_GetProcAddress("glStencilFuncSeparate");
        pgpu_stencil_mask=(PgpuStencilMaskSeparate)SDL_GL_GetProcAddress("glStencilMaskSeparate");
        pgpu_stencil_op=(PgpuStencilOpSeparate)SDL_GL_GetProcAddress("glStencilOpSeparate");
    }
    return pgpu_stencil_func && pgpu_stencil_mask && pgpu_stencil_op;
}
static PgpuVersion *pgpu_version(uint64_t id) {
    if(!id)return NULL;
    for(unsigned i=0;i<PGPU_VERSIONS;++i)
        if(s_pgpu_versions[i].id==id && s_pgpu_versions[i].references)return &s_pgpu_versions[i];
    return NULL;
}
static void pgpu_uniform_i(GLuint p,const char *name,int value) {
    p_glUniform1i(p_glGetUniformLocation(p,name),value);
}
static void pgpu_uniform_f(GLuint p,const char *name,float value) {
    p_glUniform1f(p_glGetUniformLocation(p,name),value);
}
static void pgpu_layout(GLuint *vao,GLuint *vbo,const unsigned *sizes,unsigned count,unsigned stride) {
    p_glGenVertexArrays(1,vao);p_glGenBuffers(1,vbo);
    p_glBindVertexArray(*vao);p_glBindBuffer(PSXGL_ARRAY_BUFFER,*vbo);
    unsigned offset=0;
    for(unsigned i=0;i<count;++i) {
        p_glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,stride*sizeof(float),(const void *)(uintptr_t)(offset*sizeof(float)));
        p_glEnableVertexAttribArray(i);offset+=sizes[i];
    }
}
static int pgpu_init(void) {
    if(s_pgpu_ready)return 1;
    if(s_pgpu_init_failed)return 0;
    s_pgpu_init_failed=1;
    s_pgpu_geo_prog=build_program(GEO_VS,GEO_FS);
    s_pgpu_tex_prog=build_program_ex(TEX_VS,TEX_FS,1);
    if(!s_pgpu_geo_prog || !s_pgpu_tex_prog)return 0;
    const unsigned textured[14]={2,2,4,2,2,1,1,4,1,1,1,4,1,1},flat[2]={2,4};
    pgpu_layout(&s_pgpu_tex_vao,&s_pgpu_tex_vbo,textured,14,TEXV);
    pgpu_layout(&s_pgpu_geo_vao,&s_pgpu_geo_vbo,flat,2,6);
    s_pgpu_color=make_tex(GL_RGBA8,VRAM_W,VRAM_H,GL_RGBA,GL_UNSIGNED_BYTE);
    p_glGenRenderbuffers(1,&s_pgpu_stencil);p_glBindRenderbuffer(PSXGL_RENDERBUFFER,s_pgpu_stencil);
    p_glRenderbufferStorage(PSXGL_RENDERBUFFER,PSXGL_DEPTH24_STENCIL8,VRAM_W,VRAM_H);
    p_glBindRenderbuffer(PSXGL_RENDERBUFFER,0);
    if(!make_fbo(&s_pgpu_fbo,s_pgpu_color,s_pgpu_stencil))return 0;
    s_pgpu_maskset=p_glGetUniformLocation(s_pgpu_tex_prog,"u_maskset");
    s_pgpu_semimode=p_glGetUniformLocation(s_pgpu_tex_prog,"u_semimode");
    s_pgpu_semipass=p_glGetUniformLocation(s_pgpu_tex_prog,"u_semipass");
    s_pgpu_ready=1;
    return 1;
}
uint64_t psx_presentation_gpu_snapshot(void) {
    GL_RT_SYNC("presentation_snapshot");
    if(!pgpu_available())return 0;
    flush_cpu_upload();flush_flat_batch();flush_tex_batch();pack_flush();
    for(unsigned i=0;i<PGPU_VERSIONS;++i) {
        PgpuVersion *v=&s_pgpu_versions[i];
        if(v->id && v->epoch==s_pgpu_raw_epoch) {
            if(v->references==UINT32_MAX)return 0;
            ++v->references;return v->id;
        }
    }
    PgpuVersion *v=NULL;
    for(unsigned i=0;i<PGPU_VERSIONS;++i)
        if(!s_pgpu_versions[i].references) { v=&s_pgpu_versions[i];break; }
    if(!v)return 0;
    PgpuState saved;pgpu_state_save(&saved);
    p_glBindBuffer(0x88EC,0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    if(!v->tex)v->tex=make_tex(PSXGL_R16UI,VRAM_W,VRAM_H,PSXGL_RED_INTEGER,GL_UNSIGNED_SHORT);
    if(!v->fbo && !make_fbo(&v->fbo,v->tex,0)) {
        if(v->fbo)p_glDeleteFramebuffers(1,&v->fbo);v->fbo=0;
        pgpu_state_restore(&saved);return 0;
    }
    glDisable(GL_SCISSOR_TEST);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_raw_fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER,v->fbo);
    p_glBlitFramebuffer(0,0,VRAM_W,VRAM_H,0,0,VRAM_W,VRAM_H,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    v->epoch=s_pgpu_raw_epoch;v->id=s_pgpu_next_id++;v->references=1;
    pgpu_state_restore(&saved);return v->id;
}
int psx_presentation_gpu_retain(uint64_t id) {
    GL_RT_SYNC("presentation_retain");
    PgpuVersion *v=pgpu_version(id);if(!v || v->references==UINT32_MAX)return 0;
    ++v->references;return 1;
}
void psx_presentation_gpu_release(uint64_t id) {
    GL_RT_SYNC("presentation_release");PgpuVersion *v=pgpu_version(id);if(v)--v->references;
}
static int pgpu_validate(const PsxPresentationTriangle *tri,uint32_t count,
    const PsxPresentationMaterial *mat,uint32_t materials) {
    if((count && (!tri || !mat)) || count>262144 || materials>65536)return 0;
    for(uint32_t i=0;i<count;++i) {
        if(tri[i].resource>=materials)return 0;
        const PsxPresentationMaterial *m=&mat[tri[i].resource];
        if(m->semi< -1 || m->semi>3 || m->filter>3 || (m->flags&~31u) ||
            (m->texture_window&~0xFFFFFu) || m->clip[0]<0 || m->clip[1]<0 ||
            m->clip[2]>=VRAM_W || m->clip[3]>=VRAM_H ||
            ((m->flags&PSX_PRESENTATION_TEXTURED) && (!pgpu_version(m->texture_version) ||
                ((m->texpage>>7)&3)==3 || m->clut_x>=VRAM_W || m->clut_y>=VRAM_H)))return 0;
        for(unsigned j=0;j<4;++j)if(m->uv_limits[j]>255)return 0;
        for(unsigned j=0;j<3;++j) {
            const PsxPresentationDrawVertex *v=&tri[i].vertices[j];
            if(!isfinite(v->x) || !isfinite(v->y) || fabs(v->x)>1e7 || fabs(v->y)>1e7 ||
                !isfinite(v->reciprocal_z) || v->reciprocal_z<0 || v->reciprocal_z>1e7)return 0;
            for(unsigned k=0;k<2;++k)if(!isfinite(v->uv[k]))return 0;
            for(unsigned k=0;k<4;++k)if(!isfinite(v->color[k]))return 0;
        }
    }
    return 1;
}
static void pgpu_program_state(GLuint p) {
    p_glUseProgram(p);pgpu_uniform_f(p,"u_shift",0.5f-1.0f/64.0f);
    pgpu_uniform_f(p,"u_xoff",0);pgpu_uniform_f(p,"u_xhalf",512);
    pgpu_uniform_f(p,"u_xscale",1);pgpu_uniform_f(p,"u_xcenter",0);pgpu_uniform_f(p,"u_zbias",0);
    pgpu_uniform_i(p,"u_bcls",0);pgpu_uniform_i(p,"u_badd",0);
    pgpu_uniform_i(p,"u_dither",s_dither_mode);pgpu_uniform_i(p,"u_accurate",s_accurate_blend);
}
int psx_presentation_gpu_render(const PsxPresentationTriangle *tri,uint32_t count,
    const PsxPresentationMaterial *mat,uint32_t materials) {
    GL_RT_SYNC("presentation_render");
    if(!pgpu_available() || !pgpu_validate(tri,count,mat,materials))return 0;
    PgpuState saved;pgpu_state_save(&saved);
    p_glBindBuffer(0x88EC,0);
    p_glActiveTexture(PSXGL_TEXTURE0);
    if(!pgpu_init()) { pgpu_state_restore(&saved);return 0; }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_pgpu_fbo);glViewport(0,0,VRAM_W,VRAM_H);
    glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
    glDisable(0x8DB9);glDisable(0x8C89);glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glStencilMask(0xFF);
    glClearColor(0,0,0,0);glClearStencil(0);glClear(GL_COLOR_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    pgpu_program_state(s_pgpu_geo_prog);pgpu_program_state(s_pgpu_tex_prog);
    pgpu_uniform_i(s_pgpu_tex_prog,"u_vram",0);pgpu_uniform_i(s_pgpu_tex_prog,"u_palette",1);
    pgpu_uniform_i(s_pgpu_tex_prog,"u_hd_texture",2);pgpu_uniform_i(s_pgpu_tex_prog,"u_aniso",1);
    for(uint32_t i=0;i<count;++i) {
        const PsxPresentationMaterial *m=&mat[tri[i].resource];
        if(m->clip[2]<m->clip[0] || m->clip[3]<m->clip[1])continue;
        glEnable(GL_SCISSOR_TEST);glScissor(m->clip[0],m->clip[1],m->clip[2]-m->clip[0]+1,m->clip[3]-m->clip[1]+1);
        const int mask=(m->flags&PSX_PRESENTATION_MASK_SET)!=0,check=(m->flags&PSX_PRESENTATION_MASK_CHECK)!=0;
        if(m->flags&PSX_PRESENTATION_TEXTURED) {
            float vertices[3*TEXV]={0};
            for(unsigned j=0;j<3;++j) {
                const PsxPresentationDrawVertex *v=&tri[i].vertices[j];float *out=vertices+j*TEXV;
                out[0]=(float)v->x;out[1]=(float)v->y;out[2]=v->uv[0];out[3]=v->uv[1];
                for(unsigned k=0;k<3;++k)out[4+k]=v->color[k];out[7]=1;
                out[8]=(m->texpage&15)*64;out[9]=((m->texpage>>4)&1)*256;
                out[10]=m->clut_x;out[11]=m->clut_y;out[12]=(m->texpage>>7)&3;
                out[13]=(m->flags&PSX_PRESENTATION_RAW_TEXTURE)!=0;
                for(unsigned k=0;k<4;++k)out[14+k]=m->uv_limits[k];
                out[18]=(m->semi<0?0:m->semi+1)+((m->flags&PSX_PRESENTATION_DITHER)?8:0);
                out[19]=(tri[i].flags&PSX_PRESENTATION_AFFINE_UV)?0:(float)v->reciprocal_z;
                out[20]=(float)m->texture_window;out[26]=m->filter;
            }
            p_glUseProgram(s_pgpu_tex_prog);
            p_glUniform1i(s_pgpu_maskset,mask);p_glUniform1i(s_pgpu_semimode,m->semi<0?0:m->semi);
            const PgpuVersion *version=pgpu_version(m->texture_version);
            p_glActiveTexture(PSXGL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,version->tex);
            p_glActiveTexture(PSXGL_TEXTURE0+1);glBindTexture(GL_TEXTURE_2D,version->tex);
            p_glBindVertexArray(s_pgpu_tex_vao);p_glBindBuffer(PSXGL_ARRAY_BUFFER,s_pgpu_tex_vbo);
            p_glBufferData(PSXGL_ARRAY_BUFFER,sizeof vertices,vertices,PSXGL_STREAM_DRAW);
            // Explicit STP classes keep the private stencil exact, including
            // destination masking, without deferred native-stencil state.
            glDisable(GL_BLEND);mask_stencil_ex(mask,check);
            p_glUniform1i(s_pgpu_semipass,1);glDrawArrays(GL_TRIANGLES,0,3);
            if(m->semi<0)glDisable(GL_BLEND);else apply_psx_blend(m->semi);
            mask_stencil_ex(1,check);p_glUniform1i(s_pgpu_semipass,2);
            glDrawArrays(GL_TRIANGLES,0,3);
        } else {
            float vertices[18];
            for(unsigned j=0;j<3;++j) {
                const PsxPresentationDrawVertex *v=&tri[i].vertices[j];float *out=vertices+j*6;
                out[0]=(float)v->x;out[1]=(float)v->y;
                for(unsigned k=0;k<3;++k)out[2+k]=v->color[k];
                out[5]=(float)mask+((m->flags&PSX_PRESENTATION_DITHER)?262144.0f:0);
            }
            p_glUseProgram(s_pgpu_geo_prog);
            p_glBindVertexArray(s_pgpu_geo_vao);p_glBindBuffer(PSXGL_ARRAY_BUFFER,s_pgpu_geo_vbo);
            p_glBufferData(PSXGL_ARRAY_BUFFER,sizeof vertices,vertices,PSXGL_STREAM_DRAW);
            if(m->semi<0)glDisable(GL_BLEND);else apply_psx_blend(m->semi);
            mask_stencil_ex(mask,check);glDrawArrays(GL_TRIANGLES,0,3);
        }
    }
    s_pgpu_output=1;pgpu_state_restore(&saved);return 1;
}
int psx_presentation_gpu_read_rgba(uint8_t *out,size_t capacity,int x,int y,int w,int h) {
    GL_RT_SYNC("presentation_read");
    if(!pgpu_available() || !s_pgpu_output || !out || x<0 || y<0 || w<=0 || h<=0 ||
        x>VRAM_W-w || y>VRAM_H-h || (size_t)w*h>capacity/4)return 0;
    PgpuState saved;pgpu_state_save(&saved);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_pgpu_fbo);p_glBindBuffer(0x88EB,0);
    glPixelStorei(PSXGL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,out);
    pgpu_state_restore(&saved);return 1;
}
static void presentation_gpu_shutdown(void) {
    if(s_ctx) {
        for(unsigned i=0;i<PGPU_VERSIONS;++i) {
            if(s_pgpu_versions[i].fbo)p_glDeleteFramebuffers(1,&s_pgpu_versions[i].fbo);
            if(s_pgpu_versions[i].tex)glDeleteTextures(1,&s_pgpu_versions[i].tex);
        }
        if(s_pgpu_fbo)p_glDeleteFramebuffers(1,&s_pgpu_fbo);
        if(s_pgpu_color)glDeleteTextures(1,&s_pgpu_color);
        if(s_pgpu_stencil)p_glDeleteRenderbuffers(1,&s_pgpu_stencil);
        if(s_pgpu_tex_prog)p_glDeleteProgram(s_pgpu_tex_prog);
        if(s_pgpu_geo_prog)p_glDeleteProgram(s_pgpu_geo_prog);
        if(s_pgpu_tex_vbo)p_glDeleteBuffers(1,&s_pgpu_tex_vbo);
        if(s_pgpu_geo_vbo)p_glDeleteBuffers(1,&s_pgpu_geo_vbo);
        if(s_pgpu_tex_vao)p_glDeleteVertexArrays(1,&s_pgpu_tex_vao);
        if(s_pgpu_geo_vao)p_glDeleteVertexArrays(1,&s_pgpu_geo_vao);
    }
    memset(s_pgpu_versions,0,sizeof s_pgpu_versions);
    s_pgpu_tex_prog=s_pgpu_geo_prog=s_pgpu_tex_vao=s_pgpu_geo_vao=0;
    s_pgpu_tex_vbo=s_pgpu_geo_vbo=s_pgpu_color=s_pgpu_fbo=s_pgpu_stencil=0;
    s_pgpu_output=s_pgpu_ready=s_pgpu_init_failed=0;++s_pgpu_raw_epoch;
    pgpu_stencil_func=NULL;pgpu_stencil_mask=NULL;pgpu_stencil_op=NULL;
}
int psx_presentation_gpu_write_png(const char *path,int x,int y,int w,int h) {
    if(!path || w<=0 || h<=0 || w>VRAM_W || h>VRAM_H)return 0;
    const size_t pixels=(size_t)w*h;
    uint8_t *rgba=(uint8_t *)malloc(pixels*4),*rgb=(uint8_t *)malloc(pixels*3);
    int ok=0;
    if(rgba && rgb && psx_presentation_gpu_read_rgba(rgba,pixels*4,x,y,w,h)) {
        for(size_t i=0;i<pixels;++i)memcpy(rgb+i*3,rgba+i*4,3);
        FILE *f=fopen(path,"wb");if(f) { ok=png_write_rgb(f,rgb,(uint32_t)w,(uint32_t)h);fclose(f); }
    }
    free(rgba);free(rgb);return ok;
}
int psx_presentation_gpu_test_versions(void) {
#ifndef PSX_NO_DEBUG_TOOLS
    GL_RT_SYNC("presentation_version_test");
    if(!pgpu_available())return 0;
    const uint16_t original=gr_vram_read(960,496);
    gr_vram_write(960,496,0x7FFF);const uint64_t white=psx_presentation_gpu_snapshot();
    gr_vram_write(960,496,0x001F);const uint64_t red=psx_presentation_gpu_snapshot();
    int ok=white && red && white!=red;
    // Force cache turnover while two old versions remain retained.
    for(unsigned i=0;i<PGPU_VERSIONS+1 && ok;++i) {
        gr_vram_write(960,496,(uint16_t)(0x400+i));
        const uint64_t v=psx_presentation_gpu_snapshot();ok=v!=0;
        psx_presentation_gpu_release(v);
    }
    PsxPresentationMaterial material={0};material.flags=PSX_PRESENTATION_TEXTURED|PSX_PRESENTATION_RAW_TEXTURE;
    material.texpage=0x11F;material.semi=-1;material.clip[2]=1023;material.clip[3]=511;
    material.uv_limits[1]=material.uv_limits[3]=240;
    PsxPresentationTriangle triangle={0};triangle.flags=PSX_PRESENTATION_AFFINE_UV;
    triangle.vertices[0].x=10;triangle.vertices[0].y=10;
    triangle.vertices[1].x=40;triangle.vertices[1].y=10;
    triangle.vertices[2].x=10;triangle.vertices[2].y=40;
    for(unsigned i=0;i<3;++i)triangle.vertices[i].uv[1]=240;
    uint8_t a[4]={0},b[4]={0};material.texture_version=white;
    ok=ok && psx_presentation_gpu_render(&triangle,1,&material,1) &&
        psx_presentation_gpu_read_rgba(a,sizeof a,20,20,1,1);
    material.texture_version=red;
    ok=ok && psx_presentation_gpu_render(&triangle,1,&material,1) &&
        psx_presentation_gpu_read_rgba(b,sizeof b,20,20,1,1);
    ok=ok && a[0]>240 && a[1]>240 && a[2]>240 && b[0]>240 && b[1]<8 && b[2]<8;
    psx_presentation_gpu_release(white);psx_presentation_gpu_release(red);
    gr_vram_write(960,496,original);flush_cpu_upload();
    return ok;
#else
    return 0;
#endif
}
