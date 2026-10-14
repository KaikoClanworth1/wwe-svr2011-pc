// The Simple modes' step framework (Simple superstar, Simple arena, later Simple porting): a guided page with the
// steps listed on the left (a tick when done, "!" when a step has an error), one card per step - a title, one
// "What is this?" line and only that step's controls - and Back / Next with the progress at the bottom. Each mode
// keeps its own project and draws its own cards; this file draws the frame around them, the same way everywhere.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app.h"
#include "disc_list.h"

namespace mm {
namespace simple {

struct Step {
  const char* title;
  const char* what;  // the "What is this?" line (short, plain)
};

// A big selectable card: a title and a line under it.
bool Card(const char* id, const char* title, const char* line, bool on, float w, float h);
// Cards: as many as fit a row, at least 200 wide; how many of that width fit a row.
float CardWidth();
int CardsPerRow(float w);
// A button on the heading's title line, at the right (the cursor is kept below the heading).
bool HeadingButton(const char* label, float top_y, bool accent);
// The step's title, its "What is this?" line and a separator.
void StepTitle(const Step& s);
// The step list (left, `width` wide, `bottom` left free under it): click to go there; ticks on the steps before the
// current one without errors, "!" on those with one (has_error: count entries, may be null).
void StepList(const Step* steps, int count, int& step, const bool* has_error, float width = 180, float bottom = 52);
// Back, "Step n of N" with a bar, Next (hidden on the last step).
void BottomBar(int& step, int count);
// A problem's text without the file-level details a first-timer doesn't need ("(entry 00013902)").
std::string Plain(std::string t);
// The Finish card's "Things to check": each problem with a "Fix it" button that goes to its step (p.step; none for
// steps < 0 or == finish). "Everything looks good." when there are none.
void ProblemList(const std::vector<Problem>& problems, int& step, int finish);
// A summary row on the Finish card: the step's name, the text, "Change" (goes to the step).
void SummaryRow(const Step* steps, int at, const std::string& text, int& step);
// Which steps have an error (out: count entries).
void ErrorSteps(const std::vector<Problem>& problems, int count, bool* out);
// A game picker: a card for each My game discs game `supported` takes (Games() order), with the platform and whether
// its disc is set; a game without one says so and opens My game discs when clicked. True when `key` changed.
bool GamePicker(const char* id, std::string& key, const std::function<bool(const disc_list::Game&)>& supported);

}  // namespace simple
}  // namespace mm
