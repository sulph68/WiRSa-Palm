#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <Arduino.h>

// This custom firmware does not support Over-The-Air (OTA) updates -
// firmwareUpdate()/firmwareUpdateStarted()/firmwareUpdateFinished()/
// firmwareUpdateProgress()/firmwareUpdateError() (the actual httpUpdate.h
// flash-over-HTTP path) were removed outright, not just disabled; they had
// zero live callers even before removal (the AT-command dispatcher that
// would have triggered ATFU is long gone). firmwareCheck() stays - it only
// prints a startup banner, no network call.
void firmwareCheck();

#endif // FIRMWARE_H
