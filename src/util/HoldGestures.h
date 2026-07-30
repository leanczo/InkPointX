#pragma once

// Hold-to-act durations, shared so the whole firmware teaches the user only two
// timings instead of one per screen. Before this there were six distinct values
// across nine call sites (400/500/700/900/1000/1500 ms), so holding Confirm meant
// something different in the reader, the bookmark list and the library, and no
// muscle memory could form.
namespace HoldGestures {

// Act on the thing that is selected: bookmark this page, favourite this book,
// offer to delete this entry. Short enough to feel like a deliberate press.
constexpr unsigned long SHORT_MS = 400;

// Leave, or discard: go home, jump out to the file browser, clear a text field.
// Long enough that it cannot be reached by pressing firmly.
constexpr unsigned long LONG_MS = 1000;

}  // namespace HoldGestures
