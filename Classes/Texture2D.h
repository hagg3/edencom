#ifndef Eden_Texture2D_h
#define Eden_Texture2D_h

#include <OpenGLES/ES1/gl.h>
#include <string>
#include "eden_image.h"   // G.2b-1 (de-ObjC plan D11): EdenImage, the decoded RGBA8 image (was CGImageRef)

//CONSTANTS:

typedef enum {
	kTexture2DPixelFormat_Automatic = 0,
	kTexture2DPixelFormat_RGBA8888,
	kTexture2DPixelFormat_RGBA4444,
	kTexture2DPixelFormat_RGBA5551,
	kTexture2DPixelFormat_RGB565,
	kTexture2DPixelFormat_RGB888,
	kTexture2DPixelFormat_L8,
	kTexture2DPixelFormat_A8,
	kTexture2DPixelFormat_LA88,
	kTexture2DPixelFormat_RGB_PVRTC2,
	kTexture2DPixelFormat_RGB_PVRTC4,
	kTexture2DPixelFormat_RGBA_PVRTC2,
	kTexture2DPixelFormat_RGBA_PVRTC4
} Texture2DPixelFormat;

typedef struct _Button {
    CGPoint origin;
    CGSize size;
    bool pressed=false;
}Button;
//CLASS INTERFACES:

/*
This class allows to easily create OpenGL 2D textures from images, text or raw data.
The created Texture2D object will always have power-of-two dimensions.
Depending on how you create the Texture2D object, the actual image area of the texture might be smaller than the texture dimensions i.e. "contentSize" != (pixelsWide, pixelsHigh) and (maxS, maxT) != (1.0, 1.0).
Be aware that the content of the generated textures will be upside-down!
*/
class Texture2D {

private:
	
	CGSize						_size;
	size_t						_width,
								_height;
	Texture2DPixelFormat		_format;
	GLfloat						_maxS,
								_maxT;
	// Texels per 2D-ortho unit (N.4.8). 1 for every texture except the density-built text below;
	// drawText divides the quad extent by it, so a 1 here is the stock behaviour exactly.
	GLfloat						_density = 1.0f;

    void initData(const void* data, Texture2DPixelFormat pixelFormat, int width,int height,CGSize size,bool genMips);
    // G.2b-1 (de-ObjC plan D1/D11): paths and text are std::string (never const char*: the raw-data
    // constructors take a const void*), images are EdenImage (orientation is always Up), a font is its
    // point size.
    void initFromPath(const std::string& path, bool sizeToFit, Texture2DPixelFormat pixelFormat, bool genMips);
    void initFromImage(const EdenImage* image, bool sizeToFit,Texture2DPixelFormat pixelFormat, bool genMips);
    void initFromString(const std::string& string, CGSize dimensions, UITextAlignment alignment, float fontSize);
public:
GLuint						name;
Texture2D(const void* data ,Texture2DPixelFormat pixelFormat, int width, int height, CGSize size);
~Texture2D();
Texture2D(const void* data ,Texture2DPixelFormat pixelFormat, int width, int height, CGSize size,bool genMips);
/*@property(readonly) Texture2DPixelFormat pixelFormat;
@property(readonly) NSUInteger pixelsWide;
@property(readonly) NSUInteger pixelsHigh;

@property(readonly) GLuint name;

@property(readonly, nonatomic) CGSize contentSize;
@property(readonly) GLfloat maxS;
@property(readonly) GLfloat maxT;*/


/*
Drawing extensions to make it easy to draw basic quads using a Texture2D object.
These functions require GL_TEXTURE_2D and both GL_VERTEX_ARRAY and GL_TEXTURE_COORD_ARRAY client states to be enabled.
*/

    void preload();
    void drawAtPoint(CGPoint point);
    void drawAtPoint(CGPoint point,CGFloat depth,bool center);
    void drawInRect(CGRect rect);
    void drawInRect2(CGRect rect);
    void drawText(CGRect rect);
    void drawText(CGRect rect, bool flipX);
    void drawNumbers(CGRect rect, int num);
    void drawButton(Button button);
    void drawButton(Button button, bool flipX);
    void drawButton2(Button button);
    void drawTextHalfsies(CGRect rect);
    void drawTextNoScale(CGRect rect);
    void drawTextM(CGRect rect);
    void drawInRect(CGRect rect, CGFloat depth);
    void drawSky(CGRect rect, CGFloat depth, float r, float g, float b, float a);   // G.5b-6: the caller's R::State and fog; (r,g,b,a) tints


/*
Extensions to make it easy to create a Texture2D object from an image file.
Note that RGBA type textures will have their alpha premultiplied - use the blending mode (GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
*/
Texture2D(const std::string& path ,bool sizeToFit, Texture2DPixelFormat pixelFormat,bool genMips);
Texture2D(const std::string& path ,bool sizeToFit, bool genMips);
Texture2D(const std::string& path ,bool sizeToFit);
Texture2D(const std::string& path);
    // Copies the pixels: the caller keeps (and frees) `image`.
    Texture2D(const EdenImage* image, bool sizeToFit,Texture2DPixelFormat pixelFormat,bool genMips);
    //If the path is not absolute, it is assumed to be relative to the main bundle's resources
    //For non-power-of-two images, if "sizeToFit" is YES, the image is scaled to power-of-two dimensions, otherwise extra margins are added

    
   // Texture2D(NSString* string, CGSize dimension, UITextAlignment alignment, NSString* name,CGFloat size);
    Texture2D(const std::string& string, CGSize dimension, UITextAlignment alignment, float fontSize);
    // N.4.8: the same label rasterised at `density` texels per ortho unit (font and buffer both
    // scaled) so it lands 1:1 on device pixels; it still DRAWS at `dimension`, via drawText.
    // Pass eden_ui_raster_density(). density 1 (or <= 0) is exactly the constructor above; below 1
    // (X.20, a small window on a 1x monitor) it rasterises at fewer texels, still 1:1.
    Texture2D(const std::string& string, CGSize dimension, UITextAlignment alignment, float fontSize, float density);
/*
Extensions to make it easy to create a Texture2D object from a string of text.
Note that the generated textures are of type A8 - use the blending mode (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
*/
    static void drawTexture(Texture2D* texture, CGPoint point, CGFloat depth);
    

    std::string description();
};

// Return a NEW EdenImage the caller frees (eden_image_free); null if an input is null.
EdenImage* ManipulateImagePixelData(const EdenImage* inImage,const EdenImage* inMask,int color);
EdenImage* ManipulateImagePixelData2(const EdenImage* inImage,int tint,int mode);


#endif
