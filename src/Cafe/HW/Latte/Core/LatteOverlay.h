#pragma once

void LatteOverlay_init();
void LatteOverlay_render(bool pad_view);
void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls);

void LatteOverlay_pushNotification(const std::string& text, sint32 duration);
#include "config/CemuConfig.h"

// Launcher は UI 所有値をコピーで公開し、GPU はフレーム内で同じ snapshot を使う。
struct LatteOverlaySettings
{
	decltype(CemuConfig::overlay) overlay;
	decltype(CemuConfig::notification) notification;
	uint64 revision = 0;
};
void LatteOverlay_setSettings(const LatteOverlaySettings& settings);
bool LatteOverlay_friendsEnabled();
void LatteOverlay_setControlInset(unsigned pixels);
uint64 LatteOverlay_getRenderedRevision();
