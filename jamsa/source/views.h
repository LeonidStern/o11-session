/*
    Jamsa - views.h
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

/*

  The editor's own views, drawn in code (no bitmaps), in the look of Ableton
  Live's devices: grey strips, dark value boxes, flat toggles in Live's
  yellow, orange and blue, segmented meters.

  - Strip: a rectangle of one colour (strips, panes, separators), with or without children
  - FlatButton: a toggle or a push button
  - Led: a small round light
  - DragNumberBox: Live's tempo box: drag it up or down, or double-click and type
  - MenuBox: a value box with a menu (BPI, bitrate)
  - FaderBox: a horizontal volume box, filled to the level, with its dB
  - ArcKnob: a small knob with an arc (pan, click volume)
  - SegmentMeter: a 7-segment level meter
  - BeatCells: one cell per beat of the interval
  - AddressBox: an address and its Copy button
  - TextLinesView: wrapped scrolling text (chat, license)
  - LookAndFeel: the text boxes, menus, tooltips and scrollbars JUCE draws

  Cut, Copy, Paste and Select All in the text boxes, and the clipboard, are
  JUCE's own.

*/

#ifndef _JAMSA_VIEWS_H_
#define _JAMSA_VIEWS_H_

#include <functional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace jamsa {
namespace ui {

struct Colors
{
  static juce::Colour window() { return juce::Colour(0xff2b2b2b); }
  static juce::Colour strip() { return juce::Colour(0xff363636); }
  static juce::Colour pane() { return juce::Colour(0xff1f1f1f); }
  static juce::Colour row() { return juce::Colour(0xff2a2a2a); }
  static juce::Colour myRow() { return juce::Colour(0xff33302a); }
  static juce::Colour box() { return juce::Colour(0xff1c1c1c); }
  static juce::Colour boxLine() { return juce::Colour(0xff121212); }
  static juce::Colour buttonOff() { return juce::Colour(0xff4a4a4a); }
  static juce::Colour buttonText() { return juce::Colour(0xffd0d0d0); }
  static juce::Colour separator() { return juce::Colour(0xff4a4a4a); }
  static juce::Colour label() { return juce::Colour(0xff8c8c8c); }
  static juce::Colour value() { return juce::Colour(0xffe6e6e6); }
  static juce::Colour text() { return juce::Colour(0xffc9c9c9); }
  static juce::Colour status() { return juce::Colour(0xff9a9a9a); }
  static juce::Colour placeholder() { return juce::Colour(0xff6c6c6c); }
  static juce::Colour dark() { return juce::Colour(0xff1c1c1c); } // text on a lit button
  static juce::Colour yellow() { return juce::Colour(0xffffb532); }
  static juce::Colour yellowLine() { return juce::Colour(0xffc98a1e); }
  static juce::Colour orange() { return juce::Colour(0xffff7d3b); }
  static juce::Colour orangeLine() { return juce::Colour(0xffc45a26); }
  static juce::Colour blue() { return juce::Colour(0xff6fb2ee); }
  static juce::Colour blueLine() { return juce::Colour(0xff4a86bd); }
  static juce::Colour green() { return juce::Colour(0xff5fd16a); }
  static juce::Colour purple() { return juce::Colour(0xffb99ff0); }
  static juce::Colour red() { return juce::Colour(0xffe85f55); }
  static juce::Colour pastBeat() { return juce::Colour(0xff5a4a2a); }
  static juce::Colour downbeat() { return juce::Colour(0xff262626); }
  static juce::Colour faderFill() { return juce::Colour(0xff6b6b6b); }
  static juce::Colour meterOff() { return juce::Colour(0xff2a2a2a); }
  static juce::Colour meterYellow() { return juce::Colour(0xffffd23a); }
  static juce::Colour knobRing() { return juce::Colour(0xff555555); }
};

struct Fonts
{
  juce::Font value, label, bold, small, tiny, button, chat, chatBold, mono, menu;
  Fonts(); // Arial (11 px values, 10 px labels, ...), a monospace font for addresses
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
  void setOnColors(juce::Colour fill, juce::Colour line) { m_onFill = fill; m_onLine = line; repaint(); }
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
  juce::Colour m_onFill, m_onLine;
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

// a value box that opens a menu of choices ("16 ▾")
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

// a horizontal volume box: grey up to the level, the value (dB) at the right
class FaderBox : public DragControl
{
public:
  explicit FaderBox(const juce::Font &font);
  std::function<std::string(float)> format; // the value as text
  void paint(juce::Graphics &g) override;

private:
  juce::Font m_font;
};

// a small knob: a dark disc in a grey ring, with a yellow arc from the top (bipolar: pan) or
// from the start of its travel (a volume)
class ArcKnob : public DragControl
{
public:
  explicit ArcKnob(bool bipolar);
  void paint(juce::Graphics &g) override;

private:
  bool m_bipolar;
};

// a meter of 7 segments, left to right: five green, two yellow
class SegmentMeter : public juce::Component
{
public:
  SegmentMeter() { setInterceptsMouseClicks(false, false); }
  void setPeak(float peak); // linear
  void paint(juce::Graphics &g) override;

private:
  int m_lit = 0;
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

// "LAN 192.168.1.20:2049 [Copy]": a box with a label, an address (monospace) and a Copy button;
// onCopy() when Copy is clicked
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
  juce::Font m_keyFont, m_monoFont, m_buttonFont;
  std::string m_key, m_address, m_placeholder, m_copyTitle = "Copy";
  bool m_pressed = false;
};

// lines of text, wrapped to the width, newest at the bottom; the mouse wheel scrolls back
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
} // namespace jamsa

#endif
