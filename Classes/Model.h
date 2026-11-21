#pragma once
#include "Util.h"




bool LoadModels(const char* pszReadPath);
bool UnloadModels();
bool RenderModels();
void UpdateModels(float etime);
int PointTestModels(float x,float y,float z);
void PickupModel(int idx);

void PlaceModel(int idx,Vector pos);
void ColorModel(int idx,int color);
void HitModel(int idx,Vector hitpoint);
void BurnModel(int idx);
void ExplodeModels(Vector pos,int color);
void SaveModels();
void LoadModels2();
void addMoreCreaturesIfNeeded();
// Web port dev console (row F5, project-audit-2026-07-30): spawn a creature at an exact
// position. See Model.mm's ResetModel/addMoreCreaturesIfNeeded for the fields this reuses.
bool SpawnCreatureAt(int type,Vector pos);
int CountActiveCreatures();
// K.6-4a: mod-script creature handles (ModScript.cpp; docs/entities-and-creatures.md "Script handles"). A handle is
// (slot, CreatureGen(slot) when it was made); CreatureHandleLive is true while that slot still holds that live creature.
int CreatureSlots();                               // the pool size (slots 0..n-1)
uint32_t CreatureGen(int slot);
bool CreatureHandleLive(int slot,uint32_t gen);
bool CreatureRead(int slot,int* type,Vector* pos);  // false unless the slot holds a live creature
bool RemoveCreature(int slot);                      // silent (no sound, no puff); false unless live
// K.6-4b: move a live creature (docs/entities-and-creatures.md "Script handles"). The caller refuses non-finite values.
#define CREATURE_SPEED_MAX 16.0f                    // h:setvel's clamp (blocks/s): one Move sub-step stays <= 0.4 blocks
bool CreatureVel(int slot,Vector* vel);             // false unless live
int CreatureSetPos(int slot,Vector pos);            // 0 moved; 1 not live; 2 outside the creatures' update window
bool CreatureSetVel(int slot,Vector vel);           // a launch the physics integrates; false unless live
bool CreatureKnocked(int slot);                     // a launch is still running (the AI is held off)
uint32_t CreatureSpawns(int* lastSlot);             // SpawnCreatureAt's success count and its latest slot
bool CreatureSlotFree();                            // SpawnCreatureAt would find a slot
// H.12: the third-person player body (Model.mm's PLAYER_BODY slot, outside every creature loop).
enum { PB_IDLE = 0, PB_WALK, PB_JUMP };
void UpdatePlayerBody(int type,Vector feet,float yaw,int motion,float etime);
void HidePlayerBody(void);
bool PlayerBodyAvailable(void);
bool PlayerBodyShown(Vector* pos);   // the slot is posed this frame (pos = its model origin)
// MP.5: the other players of a multiplayer session, MAX_REMOTE_BODIES slots posed like the player body
// (same rules: outside every creature loop). feet/yaw/motion as UpdatePlayerBody; type is a creature model.
#define MAX_REMOTE_BODIES 16
void UpdateRemoteBody(int slot,int type,Vector feet,float yaw,int motion,float etime);
void HideRemoteBody(int slot);
int RemoteBodiesShown(void);
void setViewNow();
void killCreature(int idx);
float wrapx(float x);
float wrapz(float z);

void CalcEnvMap(vertexObject* vert);

class MMM{
    public:
   static void ExplodeModels(Vector pos,int color);
};