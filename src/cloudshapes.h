// The shaped clouds' silhouettes: pictures in the addon's clouds folder, made into distance fields. See cloudshapes.cpp.
#pragma once

constexpr int kCloudCell      = 128;                       // texels a side of one shape's cell
constexpr int kCloudCellsSide = 4;                         // cells a side of the atlas: 16 shapes at most
constexpr int kCloudAtlas     = kCloudCell * kCloudCellsSide;
constexpr float kCloudRange   = 24.0f;                     // texels of a cell: the field's reach past the edge
constexpr int kCloudAnimals   = 8;                         // cells 0..7: the pictures (a kodo, a turtle)
constexpr int kCloudPuffFirst = 8;                         // cells 8..15: plain cumulus, made in code
constexpr int kCloudPuffs     = 8;

void CloudShapesStart();              // starts the loader thread, once
bool CloudShapesReady();              // the loader has finished (with or without shapes)
int  CloudShapesCount();              // pictures loaded, 0 until ready
int  CloudPuffCount();                // plain cumulus made, 0 until ready
const unsigned char* CloudShapesAtlas();   // kCloudAtlas x kCloudAtlas bytes, 128 = the edge, more inside
const char* CloudShapeName(int i);    // the file name without ".png"
