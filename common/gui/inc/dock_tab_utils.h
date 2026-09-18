#pragma once

namespace iqforge {

// True if `windowName` currently has input focus -- i.e. it's the window
// the user last clicked into/interacted with, regardless of whether it's
// tabbed together with other windows or standing alone in its own dock
// node. False if the window hasn't been drawn yet this session.
//
// This is deliberately focus-based rather than dock-tab-based: a window
// alone in its own dock node (nothing else tabbed with it) is trivially
// always "the selected tab" of that node, even though there's nothing to
// actually select between -- so a tab-selection check alone can't tell "the
// user is looking at this window" apart from "this window merely exists"
// once it's been dragged out of a shared tab well.
bool isWindowFocused(const char* windowName);

} // namespace iqforge
