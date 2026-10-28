// render_formats.cpp — the named vertex formats of Classes/Renderer.h, taken from the engine's own
// structs with offsetof so a struct change cannot leave a stale offset here (G.5b-0). Prefixed like the
// engine files (it includes their headers); nothing else lives here.
#include "../../../Classes/Renderer.h"
#include "../../../Classes/Graphics.h"
#include "../../../Classes/Vector.h"

#include <cstddef>

namespace R {

#define EDEN_ATTR_(T, M, ty, n) Attr{ (uint8_t)(ty), (uint8_t)(n), (uint16_t)offsetof(T, M) }
static const Attr kNone = { A_NONE, 0, 0 };

const VertexFormat kFmtTerrain = { (uint16_t)sizeof(vertexStructSmall),
    EDEN_ATTR_(vertexStructSmall, position, A_S16, 3), EDEN_ATTR_(vertexStructSmall, colors, A_U8N, 4),
    EDEN_ATTR_(vertexStructSmall, texs, A_S16, 2), kNone, kNone, kNone, kNone };

const VertexFormat kFmtObject = { (uint16_t)sizeof(vertexObject),
    EDEN_ATTR_(vertexObject, position, A_F32, 3), EDEN_ATTR_(vertexObject, colors, A_U8N, 4),
    EDEN_ATTR_(vertexObject, texs, A_F32, 2), EDEN_ATTR_(vertexObject, normal, A_F32, 3), kNone, kNone, kNone };

const VertexFormat kFmtSprite = { (uint16_t)sizeof(vertexpStruct),
    EDEN_ATTR_(vertexpStruct, position, A_F32, 3), EDEN_ATTR_(vertexpStruct, colors, A_U8N, 4),
    kNone, kNone, EDEN_ATTR_(vertexpStruct, size, A_F32, 1), kNone, kNone };

const VertexFormat kFmtBreak = { (uint16_t)sizeof(vertexpBreak),
    EDEN_ATTR_(vertexpBreak, position, A_F32, 3), EDEN_ATTR_(vertexpBreak, colors, A_U8N, 4),
    EDEN_ATTR_(vertexpBreak, texs, A_S16, 2), EDEN_ATTR_(vertexpBreak, normal, A_F32, 3), kNone, kNone, kNone };

const VertexFormat kFmtCube = { (uint16_t)sizeof(vertexStruct),
    EDEN_ATTR_(vertexStruct, position, A_F32, 3), kNone,
    EDEN_ATTR_(vertexStruct, texs, A_F32, 2), EDEN_ATTR_(vertexStruct, normal, A_F32, 3), kNone, kNone, kNone };

const VertexFormat kFmtUI2 = { 8, Attr{ A_F32, 2, 0 }, kNone, kNone, kNone, kNone, kNone, kNone };

const VertexFormat kFmtUI2UV = { 16, Attr{ A_F32, 2, 0 }, kNone, Attr{ A_F32, 2, 8 }, kNone, kNone, kNone, kNone };

const VertexFormat kFmtUI3UV = { 20, Attr{ A_F32, 3, 0 }, kNone, Attr{ A_F32, 2, 12 }, kNone, kNone, kNone, kNone };

#undef EDEN_ATTR_

}  // namespace R
