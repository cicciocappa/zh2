/* model.h — skinned glTF model + skeletal animation (soldier rendering).
 *
 * Ported from mage-commando (src/assets/model.c/h). Loads a .glb/.gltf via
 * cgltf: primitives (interleaved pos/normal/uv/joints/weights), materials
 * (base color + optional texture via stb_image), skeleton (first skin) and
 * animation clips (per-channel keyframes, translation/rotation; scale is
 * ignored — fine for Mixamo rigs). AnimState = per-instance playback with
 * crossfade blending between clips (bone-matrix lerp over blend_duration).
 *
 * All matrices in this API are column-major float[16], GL-ready and layout-
 * compatible with vat_gl.h's mat4. cglm is used INTERNALLY by model.c only
 * (deliberate: vat_gl.h also typedefs `mat4`, the two must never meet in a
 * public header).
 *
 * Deps: glad, cgltf (impl in cgltf_impl.c), stb_image (impl in stb_impl.c),
 * cglm (header-only, vat/cglm, model.c only). GL uniforms expected by
 * model_render: uVP, uModel, uSkinned, uBones[], uBaseColor, uHasTexture,
 * uTexture (see assets/shaders/skinned.vs/.fs).
 */
#ifndef MODEL_H
#define MODEL_H

#include <glad/glad.h>
#include <stdbool.h>

#define MODEL_MAX_BONES       80
#define MODEL_MAX_PRIMITIVES  16
#define MODEL_MAX_MATERIALS   16
#define MODEL_MAX_ANIM_CLIPS  32

/* Column-major 4x4, same memory layout as GL / vat_gl.h mat4 / cglm mat4. */
typedef float MdlMat4[16];

/* --- Primitive (one draw call, one material) --- */
typedef struct {
    GLuint vao, vbo, ebo;
    int    index_count;
    int    vertex_count;
    int    material_index;
    bool   has_skeleton;
} MdlPrimitive;

/* --- Material --- */
typedef struct {
    float  base_color[4];   /* RGBA */
    GLuint texture;         /* GL texture ID, 0 if none */
    bool   has_texture;
    bool   alpha_blend;
    bool   double_sided;
} MdlMaterial;

/* --- Bone --- */
typedef struct {
    int     parent;         /* -1 for root */
    MdlMat4 inverse_bind;
    MdlMat4 bind_global;    /* inverse(inverse_bind): global bind pose, for attachments */
    MdlMat4 local_rest;     /* rest pose local transform */
    char    name[64];
} MdlBone;

/* --- Skeleton --- */
typedef struct {
    MdlBone bones[MODEL_MAX_BONES];
    int     bone_count;
} MdlSkeleton;

/* --- Animation keyframe --- */
typedef struct {
    float time;
    float value[4];         /* vec3 for translation/scale, vec4 for rotation */
} MdlKeyframe;

/* --- Animation channel (one bone, one property) --- */
typedef enum {
    MDL_ANIM_TRANSLATION = 0,
    MDL_ANIM_ROTATION    = 1,
    MDL_ANIM_SCALE       = 2,
} MdlAnimPath;

typedef struct {
    int          bone_index;
    MdlAnimPath  path;
    MdlKeyframe *keyframes;
    int          keyframe_count;
} MdlAnimChannel;

/* --- Animation clip --- */
typedef struct {
    char            name[64];
    float           duration;
    MdlAnimChannel *channels;
    int             channel_count;
} MdlAnimClip;

/* --- Animation state (per-instance) --- */
#define MODEL_BLEND_DURATION 0.2f

typedef struct {
    int   clip_index;       /* current clip, -1 if none */
    float time;             /* current playback time */
    bool  looping;
    bool  playing;

    /* Blending: crossfade from previous clip */
    int   prev_clip_index;  /* -1 if no blend active */
    float prev_time;
    bool  prev_looping;
    float blend_timer;      /* counts down from blend_duration to 0 */
    float blend_duration;

    MdlMat4 bone_matrices[MODEL_MAX_BONES]; /* final skinning matrices for shader */
} AnimState;

/* --- Complete model --- */
typedef struct {
    MdlPrimitive primitives[MODEL_MAX_PRIMITIVES];
    int          primitive_count;
    MdlMaterial  materials[MODEL_MAX_MATERIALS];
    int          material_count;
    MdlSkeleton  skeleton;
    MdlAnimClip *clips;
    int          clip_count;
    bool         has_skeleton;
    /* bind-pose AABB over all primitives (model space): auto-scale/anchor */
    float        bbox_min[3], bbox_max[3];
} Model;

/* --- API --- */

/* Load a glTF/glb model (textures decoded with stb_image, owned by the model). */
bool model_load(Model *model, const char *path);

/* Free all GPU and CPU resources. */
void model_cleanup(Model *model);

/* Render the model with the given shader (see header comment for uniforms).
 * anim may be NULL for a static (bind pose / unskinned) draw. */
void model_render(const Model *model, GLuint shader,
                  const MdlMat4 vp, const MdlMat4 model_matrix,
                  const AnimState *anim);

/* Find an animation clip by name. Returns index or -1. */
int model_find_clip(const Model *model, const char *name);

/* Find a bone by name (e.g. "mixamorig:RightHand"). Returns index or -1. */
int model_find_bone(const Model *model, const char *name);

/* Init animation state. */
void anim_state_init(AnimState *state);

/* Play a clip (no-op if the clip is already playing; crossfades otherwise). */
void anim_state_play(AnimState *state, int clip_index, bool loop);

/* Update animation: advance time, compute bone matrices. */
void anim_state_update(AnimState *state, const Model *model, float dt);

/* Current global (model-space) transform of a bone — for attaching props
 * (weapon in hand, muzzle flash). dest = bone_matrices[bone] * bind_global. */
void anim_bone_global(const AnimState *state, const Model *model,
                      int bone_index, MdlMat4 dest);

#endif
