#import "Util.h"




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
void setViewNow();
void killCreature(int idx);
float wrapx(float x);
float wrapz(float z);

void CalcEnvMap(vertexObject* vert);

class MMM{
    public:
   static void ExplodeModels(Vector pos,int color);
};