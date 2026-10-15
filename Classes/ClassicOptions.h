//
//  ClassicOptions.h
//  Eden
//
//  Stage H / H.15: the Classic generator's option tables (label, GENERATOR record value), shared by
//  the New World screen's Classic tab (H.4, NewWorldMenu.mm) and World Properties' Terrain tab
//  (WorldPropertiesMenu.mm), so the two can never offer different values. The values are the
//  GENERATOR record's fields (generator-plan.md §0 and §5.1; docs/emod-file-format.md).
//
#ifndef Eden_ClassicOptions_h
#define Eden_ClassicOptions_h

#import "TerrainGenerator.h"

struct NWOption { const char* label; int value; };
static const NWOption kScales[]={ { "Classic 1x", 4 }, { "Big 1.5x", 6 }, { "Large 2x", 8 }, { "Huge 3x", 12 }, { "Extreme 4x", 16 } };
static const NWOption kHills[]={ { "Plains", 0 }, { "Rolling", 1 }, { "Classic", 2 }, { "Rugged", 3 }, { "Wild", 4 } };
static const NWOption kCaves[]={ { "Off", CLASSIC_CAVES_OFF }, { "2011", CLASSIC_CAVES_2011 }, { "Deep", CLASSIC_CAVES_DEEP } };
static const NWOption kTrees[]={ { "None", 0 }, { "Sparse", 1 }, { "Classic", 2 }, { "Dense", 3 } };
static const NWOption kSizes[]={ { "Extended", 0 }, { "2880 square", 1 } };

// The option whose value is v (0 if none: a value outside the table cannot come from a decoded record).
static inline int classic_option_index(const NWOption* opts,int n,int v){
    for(int i=0;i<n;i++) if(opts[i].value==v) return i;
    return 0;
}

#endif
