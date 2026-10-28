//
//  Camera.h
//  prototype
//
//  Created by Ari Ronen on 10/11/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_Camera_h
#define Eden_Camera_h



#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>
#include "glu.h"
#include "Util.h"

class Camera {
public:
    Camera();
    ~Camera();
    void reset();
    bool update(float etime);
    void render();
    void render2();
    // H.12: the one place the (eye, target) pair is derived from px/py/pz/yaw/pitch/mode/dist,
    // in absolute world coords. render() feeds it to the frustum and (minus chunkOffset) to the
    // scene; render2() feeds it to the picking matrix, so the three can never disagree again.
    void viewPoints(Vector* eye,Vector* at);
    void viewDir(Vector* d);   // the normalized first-person look direction
    void updateThirdPerson(float etime);   // occlusion pull-in of dist (called from Player::update)
    
	float px,py,pz;
	float vx,vy,vz;
	Vector look;
	float yaw,pitch;	
	float speed;
	int mode;    // 0 first person; H.12: 1 third person behind, 2 third person front (= CAMERA_VIEW)
	float dist;  // H.12: third-person eye distance actually in use after pull-in (CAM_TP_DIST max)
};

#endif