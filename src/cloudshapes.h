// The shaped clouds' silhouettes: pictures in the addon's clouds folder, made into distance fields. See cloudshapes.cpp.
#pragma once

constexpr int kCloudCell      = 128;                       // texels a side of one shape's cell
constexpr int kCloudCellsSide = 4;                         // cells a side of the atlas
constexpr int kCloudAtlas     = kCloudCell * kCloudCellsSide;
constexpr float kCloudRange   = 24.0f;                     // texels of a cell: the field's reach past the edge
constexpr float kCloudCutRange = 32.0f;                    // texels of a cell: the cutout's distance reach, each way
constexpr int kCloudAnimals   = 16;                        // the pictures (a kodo, a turtle): 8 until 2026-10-10,
                                                           // when cells 8..15 held plain clouds made in code

void CloudShapesStart();              // starts the loader thread, once
bool CloudShapesReady();              // the loader has finished (with or without shapes)
int  CloudShapesCount();              // pictures loaded, 0 until ready
// kCloudAtlas x kCloudAtlas texels of two bytes (as D3DFMT_A8L8: L then A). L the field: 128 = the edge, more
// inside. A the cutout round the shape (2026-10-11): an oval with puffs on its rim, as a distance, 128 = its edge,
// more inside, kCloudCutRange cell texels to either end.
const unsigned char* CloudShapesAtlas();
const char* CloudShapeName(int i);    // the file name without ".png"
