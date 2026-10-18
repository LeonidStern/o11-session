/*
    O11 Session - views.h
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The editor's own views, drawn in code (no bitmaps), in the look of Ableton
  Live's devices and Max for Live's controls: grey strips, dark value boxes,
  flat toggles in Live's yellow, orange and blue, thin meters, small type
  (Inter, built in).

  - Strip: a rectangle of one colour (strips, panes, separators), with or without children
  - FlatButton: a toggle or a push button
  - Led: a small round light
  - DragNumberBox: Live's tempo box: drag it up or down, or double-click and type
  - MenuBox: a value box with a menu (interval, codec, bitrate)
  - FaderBox: a horizontal volume box, filled to the level, with its dB
  - ArcKnob: a small dial with an arc (pan, click volume)
  - LevelMeter: a thin level meter with a peak mark and a clip light
  - NameCell: a name, its channel and a short status in colour, cut to fit
  - Chip: a coloured note with a name at the right (a change the room waits to make)
  - EventList: lines and how long ago, newest first; the mouse wheel scrolls
  - BeatCells: one cell per beat of the interval
  - AddressBox: an address to copy, its label and a Copy button
  - TextLinesView: wrapped scrolling text (the server's licence)
  - LookAndFeel: the text boxes, menus, tooltips and scrollbars JUCE draws

  Cut, Copy, Paste and Select All in the text boxes, and the clipboard, are
  JUCE's own.

*/

#ifndef _O11_VIEWS_H_
#define _O11_VIEWS_H_

#include <functional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace o11 {
namespace ui {

struct Colors
{
  static juce::Colour window() { return juce::Colour(0xff242424); }
  static juce::Colour strip() { return juce::Colour(0xff2f2f2f); }
  static juce::Colour pane() { return juce::Colour(0xff1f1f1f); }
  static juce::Colour myRow() { return juce::Colour(0xff30291f); }
  static juce::Colour box() { return juce::Colour(0xff171717); }
  static juce::Colour buttonOff() { return juce::Colour(0xff424242); }
  static juce::Colour buttonPressed() { return juce::Colour(0xff5e5e5e); }
  static juce::Colour buttonText() { return juce::Colour(0xffdadada); }
  static juce::Colour separator() { return juce::Colour(0xff454545); }
  static juce::Colour label() { return juce::Colour(0xff8c8c8c); }  // labels, quiet text
  static juce::Colour faint() { return juce::Colour(0xff5f5f5f); }  // placeholders, ages
  static juce::Colour value() { return juce::Colour(0xffdadada); }  // values, names
  static juce::Colour dark() { return juce::Colour(0xff161616); }   // text on a lit button
  static juce::Colour yellow() { return juce::Colour(0xffffb532); }
  static juce::Colour yellowLine() { return juce::Colour(0xffc98a1e); }
  static juce::Colour orange() { return juce::Colour(0xffff7d3b); }
  static juce::Colour blue() { return juce::Colour(0xff6fb2ee); }
  static juce::Colour green() { return juce::Colour(0xff5fd16a); }
  static juce::Colour amber() { return juce::Colour(0xffe2a333); }
  static juce::Colour red() { return juce::Colour(0xffff5c5c); }
  static juce::Colour purple() { return juce::Colour(0xffb99ff0); }
  static juce::Colour purpleDim() { return juce::Colour(0xff8f80b3); }
  static juce::Colour chip() { return juce::Colour(0xff382f4d); }
  static juce::Colour ledOff() { return juce::Colour(0xff5a5a5a); }
  static juce::Colour pastBeat() { return juce::Colour(0xff54442a); }
  static juce::Colour pastDownbeat() { return juce::Colour(0xff6b5530); }
  static juce::Colour downbeat() { return juce::Colour(0xff2b2b2b); }
  static juce::Colour faderFill() { return juce::Colour(0xff474747); }
  static juce::Colour faderHandle() { return juce::Colour(0xffa8a8a8); }
  static juce::Colour meterTrack() { return juce::Colour(0xff2b2b2b); }
  static juce::Colour peakMark() { return juce::Colour(0xffe6e6e6); }
  static juce::Colour lampOff() { return juce::Colour(0xff333333); }
  static juce::Colour knobRing() { return juce::Colour(0xff4f4f4f); }
};

// Inter, built into the plug-in (SIL Open Font License 1.1, see fonts/Inter-LICENSE.txt), at
// Max for Live's small sizes: 10 px values, 9 px labels
struct Fonts
{
  juce::Typeface::Ptr regular, semiBold;
  juce::Font title, value, valueBold, label, labelBold, small, button, tiny, text, mono, menu;
  Fonts();
};

// the width of text in a font
float textWidth(const juce::Font &font, const std::string &text);

// a rectangle of one colour, with round corners if radius > 0 (and any children on it)
class Strip : public juce::Component
{
public:
  explicit Strip(juce::Colour color, float radius = 0) : m_color(color), m_radius(radius) {}
  void setColor(juce::Colour c);
  void paint(juce::Graphics &g) override;

private:
  juce::Colour m_color;
  float m_radius;
};

// A flat button: a toggle (lit in its on colours when on) or a push button (lit while pressed).
class FlatButton : public juce::Component, public juce::SettableTooltipClient
{
public:
  enum Kind { Toggle, Push };
  FlatButton(const std::string &title, Kind kind, const juce::Font &font);
  void setTitle(const std::string &title);
  const std::string &title() const { return m_title; }
  void setOnColor(juce::Colour fill) { m_onFill = fill; repaint(); } // lit in yellow by default
  void setValue(bool on);   // a toggle's state, without onChange
  bool value() const { return m_on; }
  float preferredWidth() const;
  std::function<void()> onChange; // a toggle switched (value()), or a push button clicked
  void paint(juce::Graphics &g) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void enablementChanged() override { repaint(); }

private:
  std::string m_title;
  Kind m_kind;
  juce::Font m_font;
  juce::Colour m_onFill;
  bool m_on = false, m_pressed = false;
};

// a small round light
class Led : public juce::Component, public juce::SettableTooltipClient
{
public:
  Led() { setInterceptsMouseClicks(false, false); }
  void setColor(juce::Colour c, bool lit); // lit: with a glow
  void paint(juce::Graphics &g) override;

private:
  juce::Colour m_color = Colors::buttonOff();
  bool m_lit = false;
};

// Live's tempo box: drag it up or down for a value (onDragged() when the mouse goes up),
// or double-click and type one (onTyped() when Return is pressed). The editor sets what
// it shows.
class DragNumberBox : public juce::Label
{
public:
  explicit DragNumberBox(const juce::Font &font);
  // the value a drag starts from, and the text the box shows while not dragged or typed in
  void setShown(int value, const std::string &text);
  bool busy() const { return m_dragging || isBeingEdited(); } // being dragged or typed in
  std::function<int(int start, double dyUp, bool fine)> dragMap; // the value after a drag of dyUp pixels
  std::function<std::string(int)> format;                       // a value as the box shows it
  std::function<void(int)> onDragged;
  std::function<void(const std::string &)> onTyped;

  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void editorShown(juce::TextEditor *editor) override;
  void editorAboutToBeHidden(juce::TextEditor *editor) override;
  void textWasEdited() override;
  void enablementChanged() override;

private:
  int m_shown = 0;
  std::string m_shownText;
  bool m_dragging = false;
  int m_startY = 0, m_dragValue = 0;
};

// a value box that opens a menu of choices ("16 beats ▾")
class MenuBox : public juce::Component, public juce::SettableTooltipClient
{
public:
  explicit MenuBox(const juce::Font &font);
  void addEntry(const std::string &text) { m_entries.push_back(text); }
  void setShown(const std::string &text); // what the box says (the room's value may not be one of the choices)
  void setCurrentIndex(int index) { m_current = index; } // the checked entry, -1 for none
  int currentIndex() const { return m_current; }
  std::function<void(int)> onSelect; // an entry was picked
  void paint(juce::Graphics &g) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void enablementChanged() override { repaint(); }

private:
  std::string m_shown;
  juce::Font m_font;
  std::vector<std::string> m_entries;
  int m_current = -1;
};

// shared by FaderBox and ArcKnob: drag to change the value (0..1), double-click for the default,
// the mouse wheel in small steps, Shift for fine steps
class DragControl : public juce::Component, public juce::SettableTooltipClient
{
public:
  DragControl(bool vertical, float pixelsForRange);
  void setValue(float v); // without onChange
  float value() const { return m_value; }
  void setDefaultValue(float v) { m_default = v; }
  bool isEditing() const { return m_editing; }
  std::function<void()> onChange;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseDrag(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;
  void mouseDoubleClick(const juce::MouseEvent &e) override;
  void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override;

protected:
  void change(float v);
  bool m_vertical;
  float m_range; // pixels for the whole range (0: the view's width)
  float m_value = 0, m_default = 0, m_startValue = 0;
  juce::Point<int> m_start;
  bool m_editing = false;
};

// a horizontal volume box: grey up to the level, with a light edge there, and the value (dB) at the left
class FaderBox : public DragControl
{
public:
  explicit FaderBox(const juce::Font &font);
  std::function<std::string(float)> format; // the value as text
  void paint(juce::Graphics &g) override;

private:
  juce::Font m_font;
};

// a small dial: a grey ring with a yellow arc from the top (bipolar: pan) or from the start of
// its travel (a volume), and a pointer at the value
class ArcKnob : public DragControl
{
public:
  explicit ArcKnob(bool bipolar);
  void paint(juce::Graphics &g) override;

private:
  bool m_bipolar;
};

// A thin level meter, -60 dB to 0 dB (uitext::meterFraction): green, amber from -12 dB, red
// from -3 dB; a mark at the peak of the last moments; a light at the right that turns red when
// the level reaches 0 dB and stays red until clicked.
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
  LevelMeter() = default;
  void setLevel(float peak, double now); // the latest peak (linear), at now (seconds)
  bool clipped() const { return m_clip; }
  void paint(juce::Graphics &g) override;
  void mouseDown(const juce::MouseEvent &e) override; // puts the clip light out

private:
  float m_level = 0, m_peak = 0; // fractions of the meter
  double m_last = 0, m_peakAt = 0;
  bool m_clip = false;
};

// A name, its channel after a dot (quieter) and a short status in its colour: "Anna · guitar
// joined 12 s ago". What doesn't fit is cut: the status first, then the channel, then the name.
class NameCell : public juce::Component, public juce::SettableTooltipClient
{
public:
  NameCell(const juce::Font &nameFont, const juce::Font &statusFont, float statusGap);
  void setPlayer(const std::string &name, const std::string &channel);
  void setStatus(const std::string &status, juce::Colour color);
  void setNameColor(juce::Colour c);
  void paint(juce::Graphics &g) override;

private:
  juce::Font m_nameFont, m_statusFont;
  float m_gap;
  std::string m_name, m_channel, m_status;
  juce::Colour m_nameColor = Colors::value(), m_statusColor = Colors::label();
};

// a change the room waits to make, on purple: "BPM 100 from next interval", and who asked for it
// at the right when it is known
class Chip : public juce::Component, public juce::SettableTooltipClient
{
public:
  explicit Chip(const juce::Font &font);
  void set(const std::string &text, const std::string &who);
  void paint(juce::Graphics &g) override;
  void mouseUp(const juce::MouseEvent &e) override;

  std::function<void()> onClick; // when set, the chip is a button (a hand over it)
  void setClick(std::function<void()> click);

private:
  juce::Font m_font;
  std::string m_text, m_who;
};

// lines of what happened, newest first, each with how long ago at the right ("12 s"); a line
// that doesn't fit is cut and shows whole in its tooltip; the mouse wheel scrolls
class EventList : public juce::Component, public juce::TooltipClient
{
public:
  struct Event
  {
    std::string text;
    juce::Colour color;
    double time = 0; // when (seconds, the clock of now in setNow)
  };
  static constexpr int LINE_H = 16;
  explicit EventList(const juce::Font &font);
  void setEvents(const std::vector<Event> &events); // newest first
  void setPlaceholder(const std::string &text);       // shown when there are none
  void setNow(double now);                            // the ages are counted to now
  void paint(juce::Graphics &g) override;
  void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override;
  juce::String getTooltip() override;

private:
  int visibleLines() const;
  juce::Font m_font;
  std::vector<Event> m_events;
  std::string m_placeholder;
  double m_now = 0;
  long m_shownSecond = -1;
  int m_scroll = 0; // lines from the newest
};

// one cell per beat of the interval: the current one yellow, the ones before it brown, the
// first of each four a little lighter
class BeatCells : public juce::Component
{
public:
  BeatCells() { setInterceptsMouseClicks(false, false); }
  void setPosition(int pos, int length, int bpi); // bpi 0: empty cells
  void paint(juce::Graphics &g) override;

private:
  int m_beat = -1, m_bpi = 0;
};

// "LAN [192.168.1.20:2049] [Copy]": a label (none if empty), a box with the address and a Copy
// button; onCopy() when Copy is clicked
class AddressBox : public juce::Component, public juce::SettableTooltipClient
{
public:
  AddressBox(const Fonts &fonts, const std::string &key);
  void setAddress(const std::string &address, const std::string &placeholder); // the placeholder when there is none
  void setKey(const std::string &key); // "LAN", "WAN"
  const std::string &address() const { return m_address; }
  void setCopyTitle(const std::string &title); // "Copy", "Copied"
  float preferredWidth() const;
  std::function<void()> onCopy;
  void paint(juce::Graphics &g) override;
  void mouseDown(const juce::MouseEvent &e) override;
  void mouseUp(const juce::MouseEvent &e) override;

private:
  juce::Rectangle<float> copyRect() const;
  float boxLeft() const;
  juce::Font m_keyFont, m_monoFont, m_buttonFont;
  std::string m_key, m_address, m_placeholder, m_copyTitle = "Copy";
  bool m_pressed = false;
};

// lines of text, wrapped to the width, newest at the bottom (or a document from the top); the
// mouse wheel scrolls
class TextLinesView : public juce::Component
{
public:
  struct Line
  {
    std::string who; // drawn in whoColor before the text, if not empty
    std::string text;
    juce::Colour whoColor, textColor;
  };

  TextLinesView(const juce::Font &font, const juce::Font &boldFont);
  void setLines(const std::vector<Line> &lines);
  void setStartAtTop(bool top) { m_startAtTop = top; } // for a document rather than a log
  void setBackground(juce::Colour c) { m_background = c; }
  void scrollToStart() { m_scroll = 0; repaint(); }
  void paint(juce::Graphics &g) override;
  void resized() override { m_wrapped = false; }
  void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override;

private:
  struct Visual
  {
    std::string who, text;
    juce::Colour whoColor, textColor;
    float whoWidth = 0;
  };
  void wrap();

  juce::Font m_font, m_bold;
  std::vector<Line> m_lines;
  std::vector<Visual> m_visual;
  juce::Colour m_background = Colors::pane();
  bool m_wrapped = false, m_startAtTop = false;
  float m_wrapWidth = -1;
  int m_scroll = 0; // visual lines away from the newest (or from the top, with startAtTop)
};

// what JUCE draws itself: the text boxes' background and edge, the menus, the tooltips, the
// players' scrollbar
class LookAndFeel : public juce::LookAndFeel_V4
{
public:
  explicit LookAndFeel(const Fonts &fonts);
  void fillTextEditorBackground(juce::Graphics &g, int width, int height, juce::TextEditor &editor) override;
  void drawTextEditorOutline(juce::Graphics &g, int width, int height, juce::TextEditor &editor) override;
  juce::Font getPopupMenuFont() override { return m_fonts.menu; }
  void drawPopupMenuBackground(juce::Graphics &g, int width, int height) override;
  void drawPopupMenuItem(juce::Graphics &g, const juce::Rectangle<int> &area, bool isSeparator, bool isActive, bool isHighlighted,
                         bool isTicked, bool hasSubMenu, const juce::String &text, const juce::String &shortcutKeyText,
                         const juce::Drawable *icon, const juce::Colour *textColour) override;
  void getIdealPopupMenuItemSize(const juce::String &text, bool isSeparator, int standardMenuItemHeight, int &idealWidth,
                                 int &idealHeight) override;
  int getPopupMenuBorderSize() override { return 3; }
  void drawTooltip(juce::Graphics &g, const juce::String &text, int width, int height) override;
  juce::Rectangle<int> getTooltipBounds(const juce::String &tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
  void drawScrollbar(juce::Graphics &g, juce::ScrollBar &bar, int x, int y, int width, int height, bool isVertical, int thumbStart,
                     int thumbSize, bool isMouseOver, bool isMouseDown) override;
  juce::Font getLabelFont(juce::Label &label) override { return label.getFont(); }
  juce::Font getTextButtonFont(juce::TextButton &, int) override { return m_fonts.button; }

private:
  const Fonts &m_fonts;
};

juce::Label *makeLabel(const std::string &text, const juce::Font &font, juce::Colour color,
                       juce::Justification align = juce::Justification::centredLeft);
// a text field in a value box
juce::TextEditor *makeEdit(const juce::Font &font, const juce::String &placeholder);

// volume control position (0..1) <-> gain (0..2, 1 = 0 dB at 0.707)
inline float sliderToGain(float v) { return 2.0f * v * v; }
float gainToSlider(float gain);

} // namespace ui
} // namespace o11

#endif
