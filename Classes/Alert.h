//
//  Alert.h
//  Eden
//
//  Created by Ari Ronen on 2/26/15.
//
//

#ifndef Eden_Alert_h
#define Eden_Alert_h

extern void alert_init();
extern void showAlertWarpHome();

extern void showAlertDeleteConfirm(const char* name);
extern void showAlertWorldType();
// Stage S / S.5: shown after a `.eden` was converted to an `.emod`. "Delete" / "Keep" answer through
// FileManager::resolveOriginal(); nothing waits on it. `edenName` is the file name of the original.
extern void showAlertDeleteOriginal(const char* edenName);
// Stage S / S.5e: before a 64z (or not-yet-known-height: an archive) source is converted with the
// Settings toggle on. "Upgrade to 256" / "Keep 64" / Cancel answer through
// FileManager::answerConvertHeight(1 / 0 / -1); World::loadWorld waits on it.
extern void showAlertConvertHeight(const char* edenName, bool known64);

extern void showAlertReport();
extern void showAlertReportConfirm();
#endif
