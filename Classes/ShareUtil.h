//
//  ShareUtil.h
//  prototype
//
//  Created by Ari Ronen on 11/10/10.
//  Copyright 2010 __MyCompanyName__. All rights reserved.
//
#ifndef Eden_ShareUtil_h
#define Eden_ShareUtil_h

#include <string>

// G.2b-2 (de-ObjC plan D13): a C++ class with the stock method names (was an NSObject @interface
// over FileDownload). Every target stubs it (seam_link_stubs_native.cpp); the stock body is
// archive/stock-unbuilt/ShareUtil.mm. Get Worlds is WorldBrowser now.
class ShareUtil {
public:
    bool isPreview,isWorldlist;
    std::string listresult;
    void canceldl();
    void loadShared(const std::string& file_name);
    void reportWorld(const std::string& file_name);
    void loadSharedPreview(const std::string& file_name);
    void shareWorld(const std::string& file_name);
    void getSharedWorldList();
    std::string searchSharedWorlds(const std::string& query);
};


#endif