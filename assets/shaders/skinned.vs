#version 330 core
// Skinned mesh (soldier, model.c). Interleaved pos/normal/uv/joints/weights;
// up to 4 bone influences per vertex. uSkinned=0 draws in bind pose (props
// without a skin reuse the same shader).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aJoints;   // bone indices stored as float
layout (location = 4) in vec4 aWeights;
uniform mat4 uVP;
uniform mat4 uModel;
uniform int  uSkinned;
uniform mat4 uBones[80];                 // MODEL_MAX_BONES
out vec3 vNormal;
out vec2 vUV;
void main() {
    vec4 pos = vec4(aPos, 1.0);
    vec3 nrm = aNormal;
    if (uSkinned == 1) {
        mat4 skin = aWeights.x * uBones[int(aJoints.x)]
                  + aWeights.y * uBones[int(aJoints.y)]
                  + aWeights.z * uBones[int(aJoints.z)]
                  + aWeights.w * uBones[int(aJoints.w)];
        pos = skin * pos;
        nrm = mat3(skin) * nrm;
    }
    gl_Position = uVP * uModel * pos;
    vNormal = mat3(uModel) * nrm;   // includes uniform scale (renormalized in fs)
    vUV = aUV;
}
