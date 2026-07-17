/* model.c — skinned glTF loading + skeletal animation. See model.h.
 * cglm is used only here (never in the header: its `mat4` typedef collides
 * with vat_gl.h's). Struct fields are flat float[16]; cglm ops go through
 * properly-aligned stack locals via memcpy. */
#include "model.h"
#include <cglm/cglm.h>
#include "cgltf.h"
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void m16_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* ============================================================
 * Internal helpers
 * ============================================================ */

static char *path_directory(const char *filepath) {
    const char *last_slash = strrchr(filepath, '/');
    int len = last_slash ? (int)(last_slash - filepath) : 1;
    char *dir = malloc(len + 1);
    if (last_slash) memcpy(dir, filepath, len);
    else            dir[0] = '.';
    dir[len] = '\0';
    return dir;
}

static GLuint make_texture(const unsigned char *pixels, int w, int h) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return tex;
}

static GLuint load_texture_from_memory(const unsigned char *data, int len) {
    int w, h, ch;
    unsigned char *pixels = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!pixels) {
        fprintf(stderr, "model: stb_image failed to decode embedded texture\n");
        return 0;
    }
    GLuint tex = make_texture(pixels, w, h);
    stbi_image_free(pixels);
    return tex;
}

static GLuint load_texture_from_file(const char *path) {
    int w, h, ch;
    unsigned char *pixels = stbi_load(path, &w, &h, &ch, 4);
    if (!pixels) {
        fprintf(stderr, "model: stb_image failed to load %s\n", path);
        return 0;
    }
    GLuint tex = make_texture(pixels, w, h);
    stbi_image_free(pixels);
    return tex;
}

/* Read accessor data into a float array. Returns malloc'd buffer. */
static float *read_accessor_floats(const cgltf_accessor *acc, int components) {
    cgltf_size count = acc->count;
    float *buf = malloc(count * components * sizeof(float));
    for (cgltf_size i = 0; i < count; i++)
        cgltf_accessor_read_float(acc, i, buf + i * components, components);
    return buf;
}

static unsigned int *read_accessor_indices(const cgltf_accessor *acc) {
    cgltf_size count = acc->count;
    unsigned int *buf = malloc(count * sizeof(unsigned int));
    for (cgltf_size i = 0; i < count; i++)
        buf[i] = (unsigned int)cgltf_accessor_read_index(acc, i);
    return buf;
}

static unsigned short *read_accessor_joints(const cgltf_accessor *acc) {
    cgltf_size count = acc->count;
    unsigned short *buf = malloc(count * 4 * sizeof(unsigned short));
    for (cgltf_size i = 0; i < count; i++) {
        cgltf_uint joints[4];
        cgltf_accessor_read_uint(acc, i, joints, 4);
        for (int c = 0; c < 4; c++)
            buf[i * 4 + c] = (unsigned short)joints[c];
    }
    return buf;
}

/* Find bone index in skin's joint array for a given node */
static int find_bone_index(const cgltf_skin *skin, const cgltf_node *node) {
    for (cgltf_size i = 0; i < skin->joints_count; i++)
        if (skin->joints[i] == node) return (int)i;
    return -1;
}

/* ============================================================
 * Loading
 * ============================================================ */

static void load_materials(Model *model, cgltf_data *data, const char *dir) {
    model->material_count = 0;

    /* per-file dedup: map cgltf image index -> GL texture */
    GLuint *tex_of = calloc(data->images_count ? data->images_count : 1, sizeof(GLuint));
    bool *tex_done = calloc(data->images_count ? data->images_count : 1, sizeof(bool));

    for (cgltf_size i = 0; i < data->materials_count &&
                           model->material_count < MODEL_MAX_MATERIALS; i++) {
        cgltf_material *mat = &data->materials[i];
        MdlMaterial *m = &model->materials[model->material_count++];

        if (mat->has_pbr_metallic_roughness) {
            memcpy(m->base_color, mat->pbr_metallic_roughness.base_color_factor,
                   4 * sizeof(float));
        } else {
            m->base_color[0] = m->base_color[1] = m->base_color[2] = 1.0f;
            m->base_color[3] = 1.0f;
        }

        m->alpha_blend = (mat->alpha_mode == cgltf_alpha_mode_blend);
        m->double_sided = mat->double_sided;
        m->has_texture = false;
        m->texture = 0;

        if (mat->has_pbr_metallic_roughness &&
            mat->pbr_metallic_roughness.base_color_texture.texture) {
            cgltf_image *img = mat->pbr_metallic_roughness.base_color_texture.texture->image;
            int img_idx = (int)(img - data->images);

            if (!tex_done[img_idx]) {
                if (img->buffer_view) {
                    /* embedded texture (GLB) */
                    const unsigned char *buf_data =
                        (const unsigned char *)img->buffer_view->buffer->data +
                        img->buffer_view->offset;
                    tex_of[img_idx] = load_texture_from_memory(
                        buf_data, (int)img->buffer_view->size);
                } else if (img->uri) {
                    char fullpath[512];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, img->uri);
                    tex_of[img_idx] = load_texture_from_file(fullpath);
                }
                tex_done[img_idx] = true;
            }
            m->texture = tex_of[img_idx];
            m->has_texture = (m->texture != 0);
        }
    }

    free(tex_of);
    free(tex_done);
}

static void load_primitives(Model *model, cgltf_data *data) {
    model->primitive_count = 0;

    for (cgltf_size mi = 0; mi < data->meshes_count; mi++) {
        cgltf_mesh *mesh = &data->meshes[mi];

        for (cgltf_size pi = 0; pi < mesh->primitives_count; pi++) {
            if (model->primitive_count >= MODEL_MAX_PRIMITIVES) break;
            cgltf_primitive *prim = &mesh->primitives[pi];
            MdlPrimitive *p = &model->primitives[model->primitive_count];

            cgltf_accessor *pos_acc = NULL, *norm_acc = NULL, *uv_acc = NULL;
            cgltf_accessor *joint_acc = NULL, *weight_acc = NULL;

            for (cgltf_size ai = 0; ai < prim->attributes_count; ai++) {
                cgltf_attribute *attr = &prim->attributes[ai];
                switch (attr->type) {
                    case cgltf_attribute_type_position:  pos_acc    = attr->data; break;
                    case cgltf_attribute_type_normal:    norm_acc   = attr->data; break;
                    case cgltf_attribute_type_texcoord:  if (attr->index == 0) uv_acc = attr->data; break;
                    case cgltf_attribute_type_joints:    joint_acc  = attr->data; break;
                    case cgltf_attribute_type_weights:   weight_acc = attr->data; break;
                    default: break;
                }
            }

            if (!pos_acc) continue;
            int vert_count = (int)pos_acc->count;
            p->vertex_count = vert_count;
            p->has_skeleton = (joint_acc != NULL && weight_acc != NULL);

            float *positions = read_accessor_floats(pos_acc, 3);
            for (int v = 0; v < vert_count; v++)
                for (int c = 0; c < 3; c++) {
                    float x = positions[v * 3 + c];
                    if (x < model->bbox_min[c]) model->bbox_min[c] = x;
                    if (x > model->bbox_max[c]) model->bbox_max[c] = x;
                }
            float *normals = norm_acc ? read_accessor_floats(norm_acc, 3) : NULL;
            float *uvs = uv_acc ? read_accessor_floats(uv_acc, 2) : NULL;
            float *weights = weight_acc ? read_accessor_floats(weight_acc, 4) : NULL;
            unsigned short *joints = joint_acc ? read_accessor_joints(joint_acc) : NULL;

            /* Interleaved vertex buffer:
             * vec3 pos + vec3 normal + vec2 uv + vec4 joints(as float) + vec4 weights
             * = 16 floats per vertex */
            int stride = 16;
            float *vbuf = calloc((size_t)vert_count * stride, sizeof(float));

            for (int v = 0; v < vert_count; v++) {
                float *dst = vbuf + (size_t)v * stride;
                dst[0] = positions[v * 3 + 0];
                dst[1] = positions[v * 3 + 1];
                dst[2] = positions[v * 3 + 2];
                if (normals) {
                    dst[3] = normals[v * 3 + 0];
                    dst[4] = normals[v * 3 + 1];
                    dst[5] = normals[v * 3 + 2];
                } else {
                    dst[3] = 0; dst[4] = 1; dst[5] = 0;
                }
                if (uvs) {
                    dst[6] = uvs[v * 2 + 0];
                    dst[7] = uvs[v * 2 + 1];
                }
                if (joints) {
                    dst[8]  = (float)joints[v * 4 + 0];
                    dst[9]  = (float)joints[v * 4 + 1];
                    dst[10] = (float)joints[v * 4 + 2];
                    dst[11] = (float)joints[v * 4 + 3];
                }
                if (weights) {
                    dst[12] = weights[v * 4 + 0];
                    dst[13] = weights[v * 4 + 1];
                    dst[14] = weights[v * 4 + 2];
                    dst[15] = weights[v * 4 + 3];
                }
            }

            free(positions);
            free(normals);
            free(uvs);
            free(joints);
            free(weights);

            unsigned int *indices = NULL;
            int idx_count = 0;
            if (prim->indices) {
                idx_count = (int)prim->indices->count;
                indices = read_accessor_indices(prim->indices);
            }

            glGenVertexArrays(1, &p->vao);
            glGenBuffers(1, &p->vbo);
            glBindVertexArray(p->vao);
            glBindBuffer(GL_ARRAY_BUFFER, p->vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)vert_count * stride * sizeof(float), vbuf,
                         GL_STATIC_DRAW);

            GLsizei fstride = stride * sizeof(float);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, fstride, (void *)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, fstride,
                                  (void *)(3 * sizeof(float)));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, fstride,
                                  (void *)(6 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, fstride,
                                  (void *)(8 * sizeof(float)));
            glEnableVertexAttribArray(3);
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, fstride,
                                  (void *)(12 * sizeof(float)));
            glEnableVertexAttribArray(4);

            if (indices) {
                glGenBuffers(1, &p->ebo);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p->ebo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             (GLsizeiptr)idx_count * sizeof(unsigned int), indices,
                             GL_STATIC_DRAW);
                p->index_count = idx_count;
                free(indices);
            } else {
                p->ebo = 0;
                p->index_count = 0;
            }

            glBindVertexArray(0);
            free(vbuf);

            p->material_index = 0;
            if (prim->material)
                p->material_index = (int)(prim->material - data->materials);

            model->primitive_count++;
        }
    }
}

static void load_skeleton(Model *model, cgltf_data *data) {
    if (data->skins_count == 0) {
        model->has_skeleton = false;
        model->skeleton.bone_count = 0;
        return;
    }

    model->has_skeleton = true;
    cgltf_skin *skin = &data->skins[0]; /* use first skin */
    int bone_count = (int)skin->joints_count;
    if (bone_count > MODEL_MAX_BONES) {
        fprintf(stderr, "model: skin has %d joints, clamping to %d\n",
                bone_count, MODEL_MAX_BONES);
        bone_count = MODEL_MAX_BONES;
    }
    model->skeleton.bone_count = bone_count;

    float *ibm_data = NULL;
    if (skin->inverse_bind_matrices)
        ibm_data = read_accessor_floats(skin->inverse_bind_matrices, 16);

    for (int i = 0; i < bone_count; i++) {
        MdlBone *b = &model->skeleton.bones[i];
        cgltf_node *node = skin->joints[i];

        if (node->name)
            snprintf(b->name, sizeof(b->name), "%s", node->name);
        else
            snprintf(b->name, sizeof(b->name), "bone_%d", i);

        b->parent = node->parent ? find_bone_index(skin, node->parent) : -1;

        if (ibm_data)
            memcpy(b->inverse_bind, ibm_data + i * 16, 16 * sizeof(float));
        else
            m16_identity(b->inverse_bind);
        {
            mat4 mi, mo;
            memcpy(mi, b->inverse_bind, sizeof(mat4));
            glm_mat4_inv(mi, mo);
            memcpy(b->bind_global, mo, sizeof(mat4));
        }

        if (node->has_matrix) {
            memcpy(b->local_rest, node->matrix, 16 * sizeof(float));
        } else {
            mat4 T, R, S, L;
            glm_mat4_identity(T);
            glm_mat4_identity(R);
            glm_mat4_identity(S);

            if (node->has_translation) {
                vec3 trans;
                memcpy(trans, node->translation, sizeof(vec3));
                glm_translate(T, trans);
            }
            if (node->has_rotation) {
                versor q;
                memcpy(q, node->rotation, sizeof(versor));
                glm_quat_mat4(q, R);
            }
            if (node->has_scale) {
                vec3 scl;
                memcpy(scl, node->scale, sizeof(vec3));
                glm_scale(S, scl);
            }

            /* T * R * S */
            glm_mat4_mul(T, R, L);
            glm_mat4_mul(L, S, L);
            memcpy(b->local_rest, L, sizeof(mat4));
        }
    }

    free(ibm_data);
}

static void load_animations(Model *model, cgltf_data *data) {
    if (data->animations_count == 0 || !model->has_skeleton) {
        model->clips = NULL;
        model->clip_count = 0;
        return;
    }

    cgltf_skin *skin = &data->skins[0];
    int clip_count = (int)data->animations_count;
    if (clip_count > MODEL_MAX_ANIM_CLIPS) clip_count = MODEL_MAX_ANIM_CLIPS;

    model->clips = calloc(clip_count, sizeof(MdlAnimClip));
    model->clip_count = clip_count;

    for (int ci = 0; ci < clip_count; ci++) {
        cgltf_animation *anim = &data->animations[ci];
        MdlAnimClip *clip = &model->clips[ci];

        if (anim->name)
            snprintf(clip->name, sizeof(clip->name), "%s", anim->name);
        else
            snprintf(clip->name, sizeof(clip->name), "clip_%d", ci);

        clip->duration = 0.0f;
        clip->channel_count = (int)anim->channels_count;
        clip->channels = calloc(clip->channel_count, sizeof(MdlAnimChannel));

        for (int chi = 0; chi < clip->channel_count; chi++) {
            cgltf_animation_channel *src = &anim->channels[chi];
            MdlAnimChannel *dst = &clip->channels[chi];

            dst->bone_index = find_bone_index(skin, src->target_node);

            switch (src->target_path) {
                case cgltf_animation_path_type_translation: dst->path = MDL_ANIM_TRANSLATION; break;
                case cgltf_animation_path_type_rotation:    dst->path = MDL_ANIM_ROTATION;    break;
                case cgltf_animation_path_type_scale:       dst->path = MDL_ANIM_SCALE;       break;
                default: dst->path = MDL_ANIM_TRANSLATION; break;
            }

            cgltf_animation_sampler *sampler = src->sampler;
            int kf_count = (int)sampler->input->count;
            dst->keyframe_count = kf_count;
            dst->keyframes = calloc(kf_count, sizeof(MdlKeyframe));

            float *times = read_accessor_floats(sampler->input, 1);
            int components = (dst->path == MDL_ANIM_ROTATION) ? 4 : 3;
            float *values = read_accessor_floats(sampler->output, components);

            for (int k = 0; k < kf_count; k++) {
                dst->keyframes[k].time = times[k];
                for (int c = 0; c < components; c++)
                    dst->keyframes[k].value[c] = values[k * components + c];
                if (times[k] > clip->duration)
                    clip->duration = times[k];
            }

            free(times);
            free(values);
        }
    }
}

/* ============================================================
 * Public API
 * ============================================================ */

bool model_load(Model *model, const char *path) {
    memset(model, 0, sizeof(Model));
    for (int c = 0; c < 3; c++) {
        model->bbox_min[c] =  1e30f;
        model->bbox_max[c] = -1e30f;
    }

    cgltf_options options = {0};
    cgltf_data *data = NULL;

    cgltf_result result = cgltf_parse_file(&options, path, &data);
    if (result != cgltf_result_success) {
        fprintf(stderr, "model: cgltf failed to parse %s (error %d)\n", path, result);
        return false;
    }

    result = cgltf_load_buffers(&options, data, path);
    if (result != cgltf_result_success) {
        fprintf(stderr, "model: cgltf failed to load buffers for %s\n", path);
        cgltf_free(data);
        return false;
    }

    char *dir = path_directory(path);

    load_materials(model, data, dir);
    load_primitives(model, data);
    load_skeleton(model, data);
    load_animations(model, data);

    free(dir);
    cgltf_free(data);

    if (model->bbox_min[0] > model->bbox_max[0]) {   /* no vertices seen */
        memset(model->bbox_min, 0, sizeof(model->bbox_min));
        memset(model->bbox_max, 0, sizeof(model->bbox_max));
    }

    printf("model: loaded %s (%d primitives, %d materials, %d bones, %d clips)\n",
           path, model->primitive_count, model->material_count,
           model->skeleton.bone_count, model->clip_count);

    return true;
}

void model_cleanup(Model *model) {
    for (int i = 0; i < model->primitive_count; i++) {
        MdlPrimitive *p = &model->primitives[i];
        glDeleteVertexArrays(1, &p->vao);
        glDeleteBuffers(1, &p->vbo);
        if (p->ebo) glDeleteBuffers(1, &p->ebo);
    }
    for (int i = 0; i < model->material_count; i++)
        if (model->materials[i].texture)
            glDeleteTextures(1, &model->materials[i].texture);
    if (model->clips) {
        for (int i = 0; i < model->clip_count; i++) {
            for (int c = 0; c < model->clips[i].channel_count; c++)
                free(model->clips[i].channels[c].keyframes);
            free(model->clips[i].channels);
        }
        free(model->clips);
    }
    memset(model, 0, sizeof(Model));
}

void model_render(const Model *model, GLuint shader,
                  const MdlMat4 vp, const MdlMat4 model_matrix,
                  const AnimState *anim) {
    glUseProgram(shader);

    GLint loc_vp = glGetUniformLocation(shader, "uVP");
    GLint loc_model = glGetUniformLocation(shader, "uModel");
    GLint loc_skinned = glGetUniformLocation(shader, "uSkinned");
    GLint loc_base_color = glGetUniformLocation(shader, "uBaseColor");
    GLint loc_has_tex = glGetUniformLocation(shader, "uHasTexture");
    GLint loc_tex = glGetUniformLocation(shader, "uTexture");

    glUniformMatrix4fv(loc_vp, 1, GL_FALSE, vp);
    glUniformMatrix4fv(loc_model, 1, GL_FALSE, model_matrix);

    bool skinned = model->has_skeleton && anim;
    glUniform1i(loc_skinned, skinned ? 1 : 0);
    if (skinned) {
        GLint loc_bones = glGetUniformLocation(shader, "uBones");
        glUniformMatrix4fv(loc_bones, model->skeleton.bone_count, GL_FALSE,
                           (const float *)anim->bone_matrices);
    }

    for (int i = 0; i < model->primitive_count; i++) {
        const MdlPrimitive *p = &model->primitives[i];
        bool blend = false;

        if (p->material_index < model->material_count) {
            const MdlMaterial *m = &model->materials[p->material_index];
            glUniform4fv(loc_base_color, 1, m->base_color);
            glUniform1i(loc_has_tex, m->has_texture ? 1 : 0);
            if (m->has_texture) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, m->texture);
                glUniform1i(loc_tex, 0);
            }
            if (m->alpha_blend) {
                blend = true;
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
        } else {
            glUniform4f(loc_base_color, 1, 1, 1, 1);
            glUniform1i(loc_has_tex, 0);
        }

        glBindVertexArray(p->vao);
        if (p->index_count > 0)
            glDrawElements(GL_TRIANGLES, p->index_count, GL_UNSIGNED_INT, 0);
        else
            glDrawArrays(GL_TRIANGLES, 0, p->vertex_count);

        if (blend) glDisable(GL_BLEND);
    }
    glBindVertexArray(0);
}

int model_find_clip(const Model *model, const char *name) {
    for (int i = 0; i < model->clip_count; i++)
        if (strcmp(model->clips[i].name, name) == 0) return i;
    return -1;
}

int model_find_bone(const Model *model, const char *name) {
    for (int i = 0; i < model->skeleton.bone_count; i++)
        if (strcmp(model->skeleton.bones[i].name, name) == 0) return i;
    return -1;
}

/* ============================================================
 * Animation
 * ============================================================ */

void anim_state_init(AnimState *state) {
    memset(state, 0, sizeof(AnimState));
    state->clip_index = -1;
    state->prev_clip_index = -1;
    state->playing = false;
    state->blend_timer = 0.0f;
    state->blend_duration = MODEL_BLEND_DURATION;
    for (int i = 0; i < MODEL_MAX_BONES; i++)
        m16_identity(state->bone_matrices[i]);
}

void anim_state_play(AnimState *state, int clip_index, bool loop) {
    /* already playing this clip — don't restart */
    if (state->clip_index == clip_index && state->playing) return;

    /* if already playing a different clip, crossfade from it */
    if (state->clip_index >= 0 && state->playing) {
        state->prev_clip_index = state->clip_index;
        state->prev_time = state->time;
        state->prev_looping = state->looping;
        state->blend_timer = state->blend_duration;
    }
    state->clip_index = clip_index;
    state->time = 0.0f;
    state->looping = loop;
    state->playing = true;
}

/* Interpolate between two keyframes */
static void sample_channel(const MdlAnimChannel *ch, float time,
                           float *out, int components) {
    if (ch->keyframe_count == 0) return;

    if (time <= ch->keyframes[0].time) {
        memcpy(out, ch->keyframes[0].value, components * sizeof(float));
        return;
    }
    if (time >= ch->keyframes[ch->keyframe_count - 1].time) {
        memcpy(out, ch->keyframes[ch->keyframe_count - 1].value,
               components * sizeof(float));
        return;
    }

    for (int i = 0; i < ch->keyframe_count - 1; i++) {
        float t0 = ch->keyframes[i].time;
        float t1 = ch->keyframes[i + 1].time;
        if (time >= t0 && time < t1) {
            float alpha = (time - t0) / (t1 - t0);

            if (components == 4) {
                /* quaternion slerp — copy to aligned locals for SIMD */
                versor q0, q1, result;
                memcpy(q0, ch->keyframes[i].value, sizeof(versor));
                memcpy(q1, ch->keyframes[i + 1].value, sizeof(versor));
                glm_quat_slerp(q0, q1, alpha, result);
                memcpy(out, result, sizeof(versor));
            } else {
                for (int c = 0; c < components; c++)
                    out[c] = ch->keyframes[i].value[c] * (1.0f - alpha) +
                             ch->keyframes[i + 1].value[c] * alpha;
            }
            return;
        }
    }
}

/* Compute bone matrices for a single clip at a given time.
 * Writes final matrices (global * inverse_bind) into out_matrices. */
static void compute_pose(const Model *model, int clip_index, float time,
                         MdlMat4 out_matrices[MODEL_MAX_BONES]) {
    const MdlSkeleton *skel = &model->skeleton;
    const MdlAnimClip *clip = &model->clips[clip_index];

    mat4 local_transforms[MODEL_MAX_BONES];
    for (int i = 0; i < skel->bone_count; i++)
        memcpy(local_transforms[i], skel->bones[i].local_rest, sizeof(mat4));

    for (int c = 0; c < clip->channel_count; c++) {
        const MdlAnimChannel *ch = &clip->channels[c];
        if (ch->bone_index < 0 || ch->bone_index >= skel->bone_count) continue;

        switch (ch->path) {
            case MDL_ANIM_TRANSLATION: {
                vec3 trans = {0, 0, 0};
                sample_channel(ch, time, trans, 3);
                local_transforms[ch->bone_index][3][0] = trans[0];
                local_transforms[ch->bone_index][3][1] = trans[1];
                local_transforms[ch->bone_index][3][2] = trans[2];
                break;
            }
            case MDL_ANIM_ROTATION: {
                versor quat = {0, 0, 0, 1};
                sample_channel(ch, time, quat, 4);
                vec3 t = {
                    local_transforms[ch->bone_index][3][0],
                    local_transforms[ch->bone_index][3][1],
                    local_transforms[ch->bone_index][3][2],
                };
                mat4 R;
                glm_quat_mat4(quat, R);
                for (int r = 0; r < 3; r++)
                    for (int col = 0; col < 3; col++)
                        local_transforms[ch->bone_index][r][col] = R[r][col];
                local_transforms[ch->bone_index][3][0] = t[0];
                local_transforms[ch->bone_index][3][1] = t[1];
                local_transforms[ch->bone_index][3][2] = t[2];
                break;
            }
            case MDL_ANIM_SCALE:
                break;
        }
    }

    /* global transforms (parents come before children in the joint array) */
    mat4 global_transforms[MODEL_MAX_BONES];
    for (int i = 0; i < skel->bone_count; i++) {
        if (skel->bones[i].parent >= 0)
            glm_mat4_mul(global_transforms[skel->bones[i].parent],
                         local_transforms[i], global_transforms[i]);
        else
            glm_mat4_copy(local_transforms[i], global_transforms[i]);
    }

    for (int i = 0; i < skel->bone_count; i++) {
        mat4 ibm, fin;
        memcpy(ibm, skel->bones[i].inverse_bind, sizeof(mat4));
        glm_mat4_mul(global_transforms[i], ibm, fin);
        memcpy(out_matrices[i], fin, sizeof(mat4));
    }
}

/* Advance clip time, handling looping */
static float advance_clip_time(float time, float duration, bool looping, float dt,
                               bool *still_playing) {
    time += dt;
    if (time > duration) {
        if (looping && duration > 0.0f) {
            time = fmodf(time, duration);
        } else {
            time = duration;
            *still_playing = false;
        }
    }
    return time;
}

void anim_state_update(AnimState *state, const Model *model, float dt) {
    if (!state->playing || state->clip_index < 0) return;
    if (state->clip_index >= model->clip_count) return;

    const MdlSkeleton *skel = &model->skeleton;

    const MdlAnimClip *clip = &model->clips[state->clip_index];
    state->time = advance_clip_time(state->time, clip->duration,
                                    state->looping, dt, &state->playing);

    compute_pose(model, state->clip_index, state->time, state->bone_matrices);

    /* crossfade with previous clip */
    if (state->blend_timer > 0.0f && state->prev_clip_index >= 0 &&
        state->prev_clip_index < model->clip_count) {

        state->blend_timer -= dt;
        if (state->blend_timer <= 0.0f) {
            state->blend_timer = 0.0f;
            state->prev_clip_index = -1;
        } else {
            const MdlAnimClip *prev_clip = &model->clips[state->prev_clip_index];
            bool prev_playing = true;
            state->prev_time = advance_clip_time(state->prev_time, prev_clip->duration,
                                                 state->prev_looping, dt, &prev_playing);

            MdlMat4 prev_matrices[MODEL_MAX_BONES];
            compute_pose(model, state->prev_clip_index, state->prev_time, prev_matrices);

            /* 0 = fully previous, 1 = fully current */
            float alpha = 1.0f - (state->blend_timer / state->blend_duration);

            for (int i = 0; i < skel->bone_count; i++)
                for (int k = 0; k < 16; k++)
                    state->bone_matrices[i][k] =
                        prev_matrices[i][k] * (1.0f - alpha) +
                        state->bone_matrices[i][k] * alpha;
        }
    }
}

void anim_bone_global(const AnimState *state, const Model *model,
                      int bone_index, MdlMat4 dest) {
    if (bone_index < 0 || bone_index >= model->skeleton.bone_count) {
        m16_identity(dest);
        return;
    }
    /* final = global * inverse_bind  =>  global = final * bind_global */
    mat4 fin, bind, out;
    memcpy(fin, state->bone_matrices[bone_index], sizeof(mat4));
    memcpy(bind, model->skeleton.bones[bone_index].bind_global, sizeof(mat4));
    glm_mat4_mul(fin, bind, out);
    memcpy(dest, out, sizeof(mat4));
}
