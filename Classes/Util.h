//
//  Util.h
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Util_h
#define Eden_Util_h


//#import <UIKit/UIKit.h>
#include "Texture2D.h"
#include "Vector.h"
#include <string>
#include "StrUtil.h"

// G.2b-2: the names are std::string (were retained NSString*), so a node is made with
// `new WorldNode()` (value-initialised: every other field zero, as the old malloc+memset) and freed
// with `delete`, never malloc/memset/free.
typedef struct _worldnode{
	std::string display_name;
	std::string file_name;
	// Stage S / S.5: a `.eden` with no paired `.emod` -- listed, flagged "needs conversion", and
	// converted when it is played (World::loadWorld).
	bool needs_convert;
	// S.5: an `.emod` whose original `.eden` (or archive) is kept beside it, hidden: its size, for the
	// list's "+3.8 GB original kept" badge. 0 = none.
	long long original_bytes;
	Button rect;
	Button anim;
    
	Texture2D* tex;
	struct _worldnode* next;
	struct _worldnode* prev;
}WorldNode;
typedef struct lnode{
	struct lnode* next;
	void* data;
	bool dead;
}ListNode;
typedef struct tnode{
	ListNode* dataList;
	int bounds[6];
	float rbounds[6];
	struct tnode* parent;
	bool hasChildren;
	struct tnode* children[8];
}TreeNode;
    

enum FC_MODES {
	FC_PLACE=1,
	FC_DESTROY=2
};
bool GenerateInverseMatrix4f(float i[16], const float m[16]);

typedef struct{
	int x;
	int y;
	int z;	
}Point3D;

float v_length2(Vector v);
typedef struct _face{
    Vector points[4];
    bool sharedface;
    int n_points;
    Vector normal;
}Face;



typedef struct _polyhedra{
    Face faces[6];
    Vector points[8];
    int n_faces;
    int n_points;
}Polyhedra;

int sign(float f);
float dotProduct(Vector A,Vector B);
Vector crossProduct(Vector v1, Vector v2);
Vector v_mult(Vector v,float scalar);
Vector v_add(Vector v,Vector v2);
Vector v_sub(Vector v,Vector v2);
Vector v_div(Vector v,float scalar);
bool v_equals(Vector vec,Vector v2);
float rsqrt( float number );

int lookupColor(Vector clr);

Vector MakeVector(float x,float y,float z);
Polyhedra makeBox(float left,float right,float back,float front,float bot,float top);
bool TestPoint(Polyhedra* A,float x,float y,float z);
Point3D findWorldCoords(int mx,int my,int mode);
Polyhedra makeRamp(float left,float right,float back,float front,float bot,float top,int type);
Polyhedra makeSide(float left,float right,float back,float front,float bot,float top,int type);
bool collidePolyhedra(Polyhedra A,Polyhedra B);
Vector calcCenter(Vector* points,int  n_points);
bool AxisSeparatePolygons(Vector Axis, Vector* A,int a_length, Vector* B,int b_length);
CGPoint CalculateInterval(Vector Axis, Vector* P,int p_length);

void MatrixMultiplyBy4x (float* src1, float* src2, float* dest);
void VectorMatrixMultiply(Vector* vDest, Vector vSrc,const float mat[16]);
void NormalizeVector(Vector* v);
float absf(float f);
bool inbox(float x,float y,CGRect rect);
bool inbox2(float x,float y,Button* rect);
bool inbox3(float x,float y,Button* rect);
bool interpolatev(Vector* vec,Vector final_vec,float speed,float etime);
Button ButtonMake(float x,float y,float width,float height);
CGRect RectFromButton(Button b);
Button ButtonFromRect(CGRect b);

//int threeToOne(int cx,int cy,int cz);
int getColIndex(int cx,int cz);
int twoToOne(int cx,int cz);
int twoToOneTest(int cx,int cz);
float randf(float max);
int randi(int max);
std::string genhash();
void takeScreenshot();
std::string eden_file_md5_hex(const std::string& path); // port/src/shim/foundation/md5.cpp; "" if unreadable

void RGBtoHSV( float r, float g, float b, float *h, float *s, float *v );
void HSVtoRGB( float *r, float *g, float *b, float h, float s, float v );

#define threeToOne(cx,cy,cz) (((cx+CHUNKS_PER_SIDE*50)%CHUNKS_PER_SIDE)*CHUNKS_PER_SIDE*CHUNKS_PER_COLUMN+\
((cz+CHUNKS_PER_SIDE*50)%CHUNKS_PER_SIDE)*CHUNKS_PER_COLUMN+cy)

#endif
