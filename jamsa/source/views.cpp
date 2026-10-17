/*
    Jamsa - views.cpp
    Copyright (C) 2026 and onward, Jamsa contributors

    Part of the Jamsa plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "views.h"

#include <math.h>
#include <algorithm>

#include "uitext.h"

namespace jamsa {
namespace ui {

static const float RADIUS = 2.0f;
static const double PI = 3.14159265358979323846;

float gainToSlider(float gain)
{
  if (!(gain > 0.0f)) return 0.0f;
  return std::min(1.0f, sqrtf(gain * 0.5f));
}

static juce::Font makeFont(const char *name, float points, bool bold = false)
{
  return juce::Font(juce::FontOptions(name, 11.0f, bold ? juce::Font::bold : juce::Font::plain)).withPointHeight(points);
}

Fonts::Fonts()
  : value(makeFont("Arial", 11)), label(makeFont("Arial", 10)), bold(makeFont("Arial", 11, true)), small(makeFont("Arial", 9.5f)),
    tiny(makeFont("Arial", 9)), button(makeFont("Arial", 10.5f)), chat(makeFont("Arial", 10.5f)), chatBold(makeFont("Arial", 10.5f, true)),
#if defined(__APPLE__)
    mono(makeFont("Menlo", 10.5f)),
#elif defined(_WIN32)
    mono(makeFont("Consolas", 10.5f)),
#else
    mono(makeFont(juce::Font::getDefaultMonospacedFontName().toRawUTF8(), 10.5f)),
#endif
    menu(makeFont("Arial", 11))
{
}

float textWidth(const juce::Font &font, const std::string &text)
{
  if (text.empty()) return 0;
  return ceilf(juce::GlyphArrangement::getStringWidth(font, juce::String::fromUTF8(text.c_str())));
}

static void fillRect(juce::Graphics &g, juce::Rectangle<float> r, float radius, juce::Colour fill, const juce::Colour *line = nullptr)
{
  g.setColour(fill);
  if (radius > 0) g.fillRoundedRectangle(r, radius);
  else g.fillRect(r);
  if (line)
  {
    g.setColour(*line);
    if (radius > 0) g.drawRoundedRectangle(r.reduced(0.5f), radius, 1.0f);
    else g.drawRect(r, 1.0f);
  }
}

// a value box: #1c1c1c with a #121212 edge
static void drawBox(juce::Graphics &g, juce::Rectangle<float> r)
{
  const juce::Colour line = Colors::boxLine();
  fillRect(g, r, RADIUS, Colors::box(), &line);
}

static juce::String str(const std::string &s) { return juce::String::fromUTF8(s.c_str()); }

// ---------------------------------------------------------------------------

void Strip::setColor(juce::Colour c)
{
  if (c == m_color) return;
  m_color = c;
  repaint();
}

void Strip::paint(juce::Graphics &g)
{
  fillRect(g, getLocalBounds().toFloat(), m_radius, m_color);
}

// ---------------------------------------------------------------------------

FlatButton::FlatButton(const std::string &title, Kind kind, const juce::Font &font)
  : m_title(title), m_kind(kind), m_font(font), m_onFill(Colors::yellow()), m_onLine(Colors::yellowLine())
{
}

void FlatButton::setTitle(const std::string &title)
{
  if (title == m_title) return;
  m_title = title;
  repaint();
}

void FlatButton::setValue(bool on)
{
  if (on == m_on) return;
  m_on = on;
  repaint();
}

float FlatButton::preferredWidth() const
{
  return textWidth(m_font, m_title) + 12;
}

void FlatButton::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  const bool lit = m_kind == Toggle ? m_on : m_pressed;
  juce::Colour fill = lit ? (m_kind == Toggle ? m_onFill : juce::Colour(0xff5e5e5e)) : Colors::buttonOff();
  juce::Colour line = lit && m_kind == Toggle ? m_onLine : Colors::box();
  juce::Colour text = lit && m_kind == Toggle ? Colors::dark() : Colors::buttonText();
  if (!isEnabled())
  {
    fill = fill.withMultipliedAlpha(0.5f);
    text = text.withMultipliedAlpha(0.5f);
  }
  fillRect(g, r, RADIUS, fill, &line);
  g.setFont(m_font);
  g.setColour(text);
  g.drawText(str(m_title), getLocalBounds(), juce::Justification::centred, false);
}

void FlatButton::mouseDown(const juce::MouseEvent &e)
{
  if (!e.mods.isLeftButtonDown() || !isEnabled()) return;
  if (m_kind == Toggle)
  {
    // a toggle switches when pressed, as Live's do
    m_on = !m_on;
    repaint();
    if (onChange) onChange();
    return;
  }
  m_pressed = true;
  repaint();
}

void FlatButton::mouseDrag(const juce::MouseEvent &e)
{
  if (m_kind == Push && e.mods.isLeftButtonDown())
  {
    const bool inside = getLocalBounds().contains(e.getPosition());
    if (inside != m_pressed)
    {
      m_pressed = inside;
      repaint();
    }
  }
}

void FlatButton::mouseUp(const juce::MouseEvent &e)
{
  if (m_kind != Push) return;
  const bool clicked = m_pressed && getLocalBounds().contains(e.getPosition());
  m_pressed = false;
  repaint();
  if (clicked && onChange) onChange();
}

// ---------------------------------------------------------------------------

void Led::setColor(juce::Colour c, bool lit)
{
  if (c == m_color && lit == m_lit) return;
  m_color = c;
  m_lit = lit;
  repaint();
}

void Led::paint(juce::Graphics &g)
{
  const float d = 7;
  const juce::Point<float> c = getLocalBounds().toFloat().getCentre();
  if (m_lit)
  {
    // the glow
    for (int i = 3; i >= 1; i --)
    {
      g.setColour(m_color.withMultipliedAlpha(0.12f));
      g.fillEllipse(c.x - d / 2 - (float)i, c.y - d / 2 - (float)i, d + 2.0f * (float)i, d + 2.0f * (float)i);
    }
  }
  g.setColour(m_color);
  g.fillEllipse(c.x - d / 2, c.y - d / 2, d, d);
}

// ---------------------------------------------------------------------------

DragNumberBox::DragNumberBox(const juce::Font &font)
{
  setFont(font);
  setJustificationType(juce::Justification::centred);
  setColour(juce::Label::textColourId, Colors::value());
  setColour(juce::Label::backgroundColourId, Colors::box());
  setColour(juce::Label::outlineColourId, Colors::boxLine());
  setColour(juce::Label::textWhenEditingColourId, Colors::value());
  setColour(juce::Label::backgroundWhenEditingColourId, Colors::box());
  setColour(juce::Label::outlineWhenEditingColourId, Colors::boxLine());
  setBorderSize(juce::BorderSize<int>(0, 3, 0, 3));
  setEditable(false, true, true); // double-click to type; losing focus discards
}

void DragNumberBox::setShown(int value, const std::string &text)
{
  m_shown = value;
  m_shownText = text;
  if (busy()) return;
  const juce::String t = str(text);
  if (getText() != t) setText(t, juce::dontSendNotification);
}

void DragNumberBox::enablementChanged()
{
  setAlpha(isEnabled() ? 1.0f : 0.5f);
  if (!isEnabled() && m_dragging)
  {
    m_dragging = false;
    setColour(juce::Label::textColourId, Colors::value());
    setText(str(m_shownText), juce::dontSendNotification);
  }
}

void DragNumberBox::mouseDown(const juce::MouseEvent &e)
{
  if (!e.mods.isLeftButtonDown() || !isEnabled() || isBeingEdited() || e.getNumberOfClicks() > 1) return;
  m_dragging = true;
  m_startY = e.getPosition().y;
  m_dragValue = m_shown;
}

void DragNumberBox::mouseDrag(const juce::MouseEvent &e)
{
  if (!m_dragging) return;
  const int v = dragMap ? dragMap(m_shown, m_startY - e.getPosition().y, e.mods.isShiftDown()) : m_shown;
  if (v != m_dragValue)
  {
    m_dragValue = v;
    setText(format ? str(format(v)) : juce::String(v), juce::dontSendNotification);
    setColour(juce::Label::textColourId, Colors::yellow()); // a value not voted for yet
  }
}

void DragNumberBox::mouseUp(const juce::MouseEvent &)
{
  if (!m_dragging) return;
  m_dragging = false;
  setColour(juce::Label::textColourId, Colors::value());
  if (m_dragValue != m_shown && onDragged) onDragged(m_dragValue);
  // until the room says otherwise, the box shows what it says now
  setText(str(m_shownText), juce::dontSendNotification);
}

void DragNumberBox::mouseDoubleClick(const juce::MouseEvent &e)
{
  if (!isEnabled()) return;
  m_dragging = false;
  juce::Label::mouseDoubleClick(e); // type a value
}

void DragNumberBox::editorShown(juce::TextEditor *editor)
{
  editor->setFont(getFont());
  editor->setJustification(juce::Justification::centred);
  editor->setColour(juce::TextEditor::textColourId, Colors::value());
  editor->setColour(juce::TextEditor::highlightedTextColourId, Colors::dark());
  editor->setColour(juce::TextEditor::highlightColourId, Colors::yellow());
  editor->setColour(juce::TextEditor::backgroundColourId, Colors::box());
  editor->setColour(juce::TextEditor::focusedOutlineColourId, Colors::yellowLine());
  editor->setColour(juce::TextEditor::outlineColourId, Colors::boxLine());
  editor->setIndents(3, 0);
  editor->applyFontToAllText(getFont());
  editor->selectAll();
}

void DragNumberBox::editorAboutToBeHidden(juce::TextEditor *)
{
  // Return commits (textWasEdited); either way the box goes back to what the room says
  juce::MessageManager::callAsync([safe = juce::Component::SafePointer<DragNumberBox>(this)]() {
    if (safe) safe->setText(str(safe->m_shownText), juce::dontSendNotification);
  });
}

void DragNumberBox::textWasEdited()
{
  if (onTyped) onTyped(getText().toStdString());
}

// ---------------------------------------------------------------------------

MenuBox::MenuBox(const juce::Font &font) : m_font(font)
{
}

void MenuBox::setShown(const std::string &text)
{
  if (text == m_shown) return;
  m_shown = text;
  repaint();
}

void MenuBox::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  drawBox(g, r);
  const float a = isEnabled() ? 1.0f : 0.5f;
  // the text, then a small triangle
  const float arrowW = 7;
  const float tw = textWidth(m_font, m_shown);
  const float total = tw + 3 + arrowW;
  float x = r.getX() + std::max(4.0f, floorf((r.getWidth() - total) / 2));
  g.setFont(m_font);
  g.setColour(Colors::value().withMultipliedAlpha(a));
  g.drawText(str(m_shown), juce::Rectangle<float>(x, r.getY(), r.getRight() - arrowW - 2 - x, r.getHeight()), juce::Justification::centredLeft, false);
  x = std::min(x + tw + 3, r.getRight() - arrowW - 3);
  const float cy = floorf(r.getCentreY()) + 0.5f;
  juce::Path p;
  p.startNewSubPath(x, cy - 2);
  p.lineTo(x + arrowW - 1, cy - 2);
  p.lineTo(x + (arrowW - 1) / 2, cy + 2);
  p.closeSubPath();
  g.setColour(Colors::label().withMultipliedAlpha(a));
  g.fillPath(p);
}

void MenuBox::mouseDown(const juce::MouseEvent &e)
{
  if (!isEnabled() || !e.mods.isLeftButtonDown()) return;
  juce::PopupMenu menu;
  menu.setLookAndFeel(&getLookAndFeel());
  for (size_t i = 0; i < m_entries.size(); i ++) menu.addItem((int)i + 1, str(m_entries[i]), true, (int)i == m_current);
  menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMinimumWidth(getWidth()),
                     [safe = juce::Component::SafePointer<MenuBox>(this)](int result) {
    if (!safe || result <= 0) return;
    safe->m_current = result - 1;
    safe->repaint();
    if (safe->onSelect) safe->onSelect(result - 1);
  });
}

// ---------------------------------------------------------------------------

DragControl::DragControl(bool vertical, float pixelsForRange) : m_vertical(vertical), m_range(pixelsForRange)
{
}

void DragControl::setValue(float v)
{
  v = std::min(1.0f, std::max(0.0f, v));
  if (fabsf(v - m_value) < 1e-7f) return;
  m_value = v;
  repaint();
}

void DragControl::change(float v)
{
  v = std::min(1.0f, std::max(0.0f, v));
  if (fabsf(v - m_value) < 1e-7f) return;
  m_value = v;
  repaint();
  if (onChange) onChange();
}

void DragControl::mouseDown(const juce::MouseEvent &e)
{
  if (!e.mods.isLeftButtonDown() || e.getNumberOfClicks() > 1) return;
  m_start = e.getPosition();
  m_startValue = m_value;
  m_editing = true;
}

void DragControl::mouseDoubleClick(const juce::MouseEvent &e)
{
  if (!e.mods.isLeftButtonDown()) return;
  // back to the default (0 dB, the middle)
  m_editing = false;
  change(m_default);
}

void DragControl::mouseDrag(const juce::MouseEvent &e)
{
  if (!m_editing || !e.mods.isLeftButtonDown()) return;
  const float range = m_range > 0 ? m_range : (float)getWidth();
  float d = (float)(m_vertical ? m_start.y - e.getPosition().y : e.getPosition().x - m_start.x);
  if (e.mods.isShiftDown()) d /= 8; // fine
  change(m_startValue + d / range);
}

void DragControl::mouseUp(const juce::MouseEvent &)
{
  m_editing = false;
}

void DragControl::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel)
{
  float d = m_vertical ? wheel.deltaY : (wheel.deltaY != 0 ? wheel.deltaY : -wheel.deltaX);
  if (d == 0) return;
  if (wheel.isReversed) d = -d;
  const float step = 0.02f * (e.mods.isShiftDown() ? 0.1f : 1.0f);
  change(m_value + (d > 0 ? step : -step));
}

// ---------------------------------------------------------------------------

FaderBox::FaderBox(const juce::Font &font) : DragControl(false, 0), m_font(font)
{
}

void FaderBox::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  drawBox(g, r);
  juce::Rectangle<float> fill = r.reduced(1, 1);
  fill.setWidth(floorf(fill.getWidth() * value() + 0.5f));
  if (fill.getWidth() > 0)
  {
    g.setColour(Colors::faderFill());
    g.fillRect(fill);
  }
  if (format)
  {
    g.setFont(m_font);
    g.setColour(Colors::buttonText());
    g.drawText(str(format(value())), getLocalBounds().withTrimmedRight(3), juce::Justification::centredRight, false);
  }
}

// ---------------------------------------------------------------------------

ArcKnob::ArcKnob(bool bipolar) : DragControl(true, 100), m_bipolar(bipolar)
{
  setDefaultValue(bipolar ? 0.5f : gainToSlider(1.0f));
}

void ArcKnob::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  const float d = std::min(r.getWidth(), r.getHeight());
  const juce::Point<float> c = r.getCentre();
  const juce::Rectangle<float> disc(c.x - d / 2, c.y - d / 2, d, d);
  g.setColour(Colors::box());
  g.fillEllipse(disc);

  // the ring, 2 px wide, 2 px in; then the value's arc over it
  const juce::Rectangle<float> ring = disc.reduced(3, 3);
  g.setColour(Colors::knobRing());
  g.drawEllipse(ring, 2.0f);

  // angles clockwise from the top, as JUCE measures them
  const double v = value();
  double from, to;
  if (m_bipolar)
  {
    from = 0; // the top
    to = (v - 0.5) * 2 * 135;
  }
  else
  {
    from = -135; // the start of its travel, bottom left
    to = -135 + v * 270;
  }
  if (fabs(to - from) > 0.5)
  {
    juce::Path p;
    p.addCentredArc(c.x, c.y, ring.getWidth() / 2, ring.getHeight() / 2, 0, (float)(std::min(from, to) * PI / 180),
                    (float)(std::max(from, to) * PI / 180), true);
    g.setColour(Colors::yellow());
    g.strokePath(p, juce::PathStrokeType(2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
  }
  // a tick at the value
  const double a = to * PI / 180.0;
  const float rr = ring.getWidth() / 2;
  g.setColour(Colors::value());
  g.drawLine(c.x + (float)sin(a) * (rr - 3), c.y - (float)cos(a) * (rr - 3), c.x + (float)sin(a) * (rr + 1), c.y - (float)cos(a) * (rr + 1), 1.5f);
}

// ---------------------------------------------------------------------------

void SegmentMeter::setPeak(float peak)
{
  const int n = uitext::litSegments(peak);
  if (n == m_lit) return;
  m_lit = n;
  repaint();
}

void SegmentMeter::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  const float h = std::min(8.0f, r.getHeight());
  const juce::Rectangle<float> m(r.getX(), floorf(r.getCentreY() - h / 2), r.getWidth(), h);
  g.setColour(Colors::boxLine());
  g.fillRect(m);
  const int n = uitext::METER_SEGMENTS;
  const float gap = 1, inner = m.getWidth() - 2 - gap * (float)(n - 1);
  for (int i = 0; i < n; i ++)
  {
    const float x0 = m.getX() + 1 + floorf((float)i * (inner / (float)n + gap)), x1 = m.getX() + 1 + floorf((float)i * (inner / (float)n + gap) + inner / (float)n);
    juce::Colour c = Colors::meterOff();
    if (i < m_lit) c = i >= n - 2 ? Colors::meterYellow() : Colors::green();
    g.setColour(c);
    g.fillRect(juce::Rectangle<float>(x0, m.getY() + 1, x1 - x0, m.getHeight() - 2));
  }
}

// ---------------------------------------------------------------------------

void BeatCells::setPosition(int pos, int length, int bpi)
{
  bpi = bpi > 0 && bpi <= 1024 ? bpi : 0;
  const int beat = bpi && length > 0 ? std::min(bpi - 1, std::max(0, (int)((double)pos / length * bpi))) : -1;
  if (beat == m_beat && bpi == m_bpi) return;
  m_beat = beat;
  m_bpi = bpi;
  repaint();
}

void BeatCells::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  const int n = m_bpi > 0 ? m_bpi : 16;
  const float h = std::min(12.0f, r.getHeight());
  const float top = floorf(r.getCentreY() - h / 2);
  const float gap = r.getWidth() / (float)n >= 6 ? 2.0f : (r.getWidth() / (float)n >= 3 ? 1.0f : 0.0f);
  const float cellW = (r.getWidth() - gap * (float)(n - 1)) / (float)n;
  for (int b = 0; b < n; b ++)
  {
    const float x0 = floorf(r.getX() + (float)b * (cellW + gap)), x1 = floorf(r.getX() + (float)b * (cellW + gap) + cellW);
    if (x1 <= x0) continue;
    juce::Colour fill = b % 4 == 0 ? Colors::downbeat() : Colors::box();
    if (m_bpi > 0 && b < m_beat) fill = Colors::pastBeat();
    if (m_bpi > 0 && b == m_beat) fill = Colors::yellow();
    const juce::Rectangle<float> cell(x0, top, x1 - x0, h);
    g.setColour(Colors::boxLine());
    g.fillRect(cell);
    g.setColour(fill);
    g.fillRect(cell.reduced(1, 1));
  }
}

// ---------------------------------------------------------------------------

AddressBox::AddressBox(const Fonts &fonts, const std::string &key)
  : m_keyFont(fonts.small), m_monoFont(fonts.mono), m_buttonFont(fonts.label), m_key(key)
{
}

void AddressBox::setAddress(const std::string &address, const std::string &placeholder)
{
  if (address == m_address && placeholder == m_placeholder) return;
  m_address = address;
  m_placeholder = placeholder;
  repaint();
}

void AddressBox::setKey(const std::string &key)
{
  if (key == m_key) return;
  m_key = key;
  repaint();
}

void AddressBox::setCopyTitle(const std::string &title)
{
  if (title == m_copyTitle) return;
  m_copyTitle = title;
  repaint();
}

float AddressBox::preferredWidth() const
{
  const std::string shown = m_address.empty() ? m_placeholder : m_address;
  const juce::Font &font = m_address.empty() ? m_keyFont : m_monoFont;
  return 5 + textWidth(m_keyFont, m_key) + 5 + textWidth(font, shown) + 6 + textWidth(m_buttonFont, "Copied") + 10;
}

juce::Rectangle<float> AddressBox::copyRect() const
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  const float w = textWidth(m_buttonFont, "Copied") + 10;
  return juce::Rectangle<float>(r.getRight() - 1 - w, r.getY() + 1, w, r.getHeight() - 2);
}

void AddressBox::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  drawBox(g, r);
  float x = r.getX() + 5;
  g.setFont(m_keyFont);
  g.setColour(Colors::label());
  g.drawText(str(m_key), juce::Rectangle<float>(x, r.getY(), r.getRight() - x, r.getHeight()), juce::Justification::centredLeft, false);
  x += textWidth(m_keyFont, m_key) + 5;
  const juce::Rectangle<float> copy = copyRect();
  const juce::Rectangle<float> textArea(x, r.getY(), std::max(0.0f, copy.getX() - 4 - x), r.getHeight());
  if (m_address.empty())
  {
    g.setFont(m_keyFont);
    g.setColour(Colors::placeholder());
    g.drawText(str(m_placeholder), textArea, juce::Justification::centredLeft, true);
  }
  else
  {
    g.setFont(m_monoFont);
    g.setColour(Colors::value());
    g.drawText(str(m_address), textArea, juce::Justification::centredLeft, true);
  }
  // the button: off when there is nothing to copy
  const bool can = !m_address.empty();
  g.setColour(m_pressed ? juce::Colour(0xff5e5e5e) : Colors::buttonOff().withMultipliedAlpha(can ? 1.0f : 0.5f));
  g.fillRect(copy);
  g.setFont(m_buttonFont);
  g.setColour(Colors::buttonText().withMultipliedAlpha(can ? 1.0f : 0.4f));
  g.drawText(str(m_copyTitle), copy, juce::Justification::centred, false);
}

void AddressBox::mouseDown(const juce::MouseEvent &e)
{
  if (!e.mods.isLeftButtonDown() || m_address.empty() || !copyRect().contains(e.getPosition().toFloat())) return;
  m_pressed = true;
  repaint();
}

void AddressBox::mouseUp(const juce::MouseEvent &e)
{
  const bool clicked = m_pressed && copyRect().contains(e.getPosition().toFloat());
  m_pressed = false;
  repaint();
  if (clicked && onCopy) onCopy();
}

// ---------------------------------------------------------------------------

TextLinesView::TextLinesView(const juce::Font &font, const juce::Font &boldFont) : m_font(font), m_bold(boldFont)
{
}

void TextLinesView::setLines(const std::vector<Line> &lines)
{
  m_lines = lines;
  m_wrapped = false;
  repaint();
}

static size_t utf8_next(const std::string &s, size_t i)
{
  if (i >= s.size()) return s.size();
  i++;
  while (i < s.size() && ((unsigned char)s[i] & 0xc0) == 0x80) i++;
  return i;
}

void TextLinesView::wrap()
{
  const float width = (float)getWidth() - 12; // margins and the scroll mark
  m_visual.clear();
  m_wrapWidth = width;
  m_wrapped = true;
  if (width < 20) return;
  auto stringWidth = [this](const std::string &s) { return textWidth(m_font, s); };

  for (const Line &line : m_lines)
  {
    Visual first;
    first.who = line.who;
    first.whoColor = line.whoColor;
    first.textColor = line.textColor;
    float indent = 0;
    if (!line.who.empty())
    {
      first.whoWidth = textWidth(m_bold, line.who + " ");
      indent = std::min(first.whoWidth, width / 2);
    }

    // break the text into words, and the words into lines
    const std::string &t = line.text;
    std::string cur;
    float avail = width - indent;
    bool isFirst = true;
    size_t i = 0;
    auto flush = [&]() {
      Visual v = isFirst ? first : Visual();
      if (!isFirst) v.textColor = line.textColor;
      v.text = cur;
      m_visual.push_back(v);
      cur.clear();
      isFirst = false;
      avail = width;
    };
    while (i < t.size())
    {
      size_t j = i;
      while (j < t.size() && t[j] != ' ' && t[j] != '\n') j++;
      std::string word = t.substr(i, j - i);
      const std::string tryLine = cur.empty() ? word : cur + " " + word;
      if (stringWidth(tryLine) <= avail) cur = tryLine;
      else
      {
        if (!cur.empty()) flush();
        // a word longer than the line is split where it fits
        while (!word.empty() && stringWidth(word) > avail)
        {
          size_t k = utf8_next(word, 0), fit = k;
          while (k < word.size())
          {
            const size_t n = utf8_next(word, k);
            if (stringWidth(word.substr(0, n)) > avail) break;
            fit = k = n;
          }
          cur = word.substr(0, fit);
          word = word.substr(fit);
          flush();
        }
        cur = word;
      }
      if (j < t.size() && t[j] == '\n') flush();
      i = j + 1;
    }
    if (!cur.empty() || isFirst) flush();
  }
}

void TextLinesView::paint(juce::Graphics &g)
{
  const juce::Rectangle<float> r = getLocalBounds().toFloat();
  g.setColour(m_background);
  g.fillRect(r);

  if (!m_wrapped || m_wrapWidth != r.getWidth() - 12) wrap();

  const float lineH = std::max(12.0f, ceilf(m_font.getHeight() * 1.18f));
  const int rows = std::max(1, (int)((r.getHeight() - 4) / lineH));
  const int total = (int)m_visual.size();
  const int maxScroll = std::max(0, total - rows);
  if (m_scroll > maxScroll) m_scroll = maxScroll;
  if (m_scroll < 0) m_scroll = 0;

  int first;
  if (m_startAtTop) first = m_scroll;
  else first = std::max(0, total - rows - m_scroll);
  const int last = std::min(total, first + rows);

  // newest at the bottom: a log starts at the bottom when it doesn't fill the view
  float y = r.getY() + 2;
  if (!m_startAtTop && total < rows) y = r.getBottom() - 2 - lineH * (float)total;
  for (int i = first; i < last; i ++, y += lineH)
  {
    const Visual &v = m_visual[(size_t)i];
    float x = r.getX() + 4;
    if (!v.who.empty())
    {
      g.setFont(m_bold);
      g.setColour(v.whoColor);
      g.drawText(str(v.who), juce::Rectangle<float>(x, y, r.getRight() - 6 - x, lineH), juce::Justification::centredLeft, false);
      x += v.whoWidth;
    }
    g.setFont(m_font);
    g.setColour(v.textColor);
    g.drawText(str(v.text), juce::Rectangle<float>(x, y, std::max(0.0f, r.getRight() - 6 - x), lineH), juce::Justification::centredLeft, false);
  }

  // where we are, when not everything fits
  if (total > rows)
  {
    const float trackH = r.getHeight() - 4;
    const float thumbH = std::max(12.0f, trackH * (float)rows / (float)total);
    const double at = maxScroll ? (m_startAtTop ? (double)m_scroll / maxScroll : 1.0 - (double)m_scroll / maxScroll) : 1.0;
    const float top = r.getY() + 2 + (trackH - thumbH) * (float)at;
    g.setColour(Colors::separator());
    g.fillRect(juce::Rectangle<float>(r.getRight() - 4, top, 2, thumbH));
  }
}

void TextLinesView::mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &wheel)
{
  if (wheel.deltaY == 0) return;
  double d = wheel.deltaY;
  if (wheel.isReversed) d = -d;
  int steps = (int)lround(d * 10);
  if (!steps) steps = d > 0 ? 1 : -1;
  m_scroll += m_startAtTop ? -steps : steps;
  if (m_scroll < 0) m_scroll = 0;
  repaint();
}

// ---------------------------------------------------------------------------

LookAndFeel::LookAndFeel(const Fonts &fonts) : m_fonts(fonts)
{
  setColour(juce::TextEditor::textColourId, Colors::value());
  setColour(juce::TextEditor::highlightedTextColourId, Colors::dark());
  setColour(juce::TextEditor::highlightColourId, Colors::yellow());
  setColour(juce::CaretComponent::caretColourId, Colors::value());
  setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xfa262626));
  setColour(juce::PopupMenu::textColourId, Colors::value());
  setColour(juce::PopupMenu::highlightedBackgroundColourId, Colors::yellow());
  setColour(juce::PopupMenu::highlightedTextColourId, Colors::dark());
  setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff262626));
  setColour(juce::TooltipWindow::textColourId, Colors::value());
  setColour(juce::TooltipWindow::outlineColourId, Colors::separator());
  setColour(juce::ScrollBar::thumbColourId, Colors::separator());
}

void LookAndFeel::fillTextEditorBackground(juce::Graphics &g, int width, int height, juce::TextEditor &)
{
  g.setColour(Colors::box());
  g.fillRoundedRectangle(juce::Rectangle<float>(0, 0, (float)width, (float)height), RADIUS);
}

void LookAndFeel::drawTextEditorOutline(juce::Graphics &g, int width, int height, juce::TextEditor &editor)
{
  g.setColour(editor.hasKeyboardFocus(true) ? Colors::yellowLine() : Colors::boxLine());
  g.drawRoundedRectangle(juce::Rectangle<float>(0, 0, (float)width, (float)height).reduced(0.5f), RADIUS, 1.0f);
}

void LookAndFeel::drawPopupMenuBackground(juce::Graphics &g, int width, int height)
{
  g.setColour(findColour(juce::PopupMenu::backgroundColourId));
  g.fillRoundedRectangle(juce::Rectangle<float>(0, 0, (float)width, (float)height), RADIUS);
  g.setColour(Colors::separator());
  g.drawRoundedRectangle(juce::Rectangle<float>(0, 0, (float)width, (float)height).reduced(0.5f), RADIUS, 1.0f);
}

void LookAndFeel::drawPopupMenuItem(juce::Graphics &g, const juce::Rectangle<int> &area, bool isSeparator, bool isActive, bool isHighlighted,
                                    bool isTicked, bool, const juce::String &text, const juce::String &, const juce::Drawable *,
                                    const juce::Colour *)
{
  if (isSeparator)
  {
    g.setColour(Colors::separator());
    g.fillRect(area.reduced(4, 0).withHeight(1).withY(area.getCentreY()));
    return;
  }
  if (isHighlighted && isActive)
  {
    g.setColour(Colors::yellow());
    g.fillRect(area.reduced(1, 0));
  }
  g.setFont(m_fonts.menu);
  g.setColour(!isActive ? Colors::label() : isHighlighted ? Colors::dark() : Colors::value());
  juce::Rectangle<int> r = area.reduced(4, 0);
  if (isTicked) g.drawText(juce::String::fromUTF8("\xe2\x9c\x93"), r.removeFromLeft(12), juce::Justification::centredLeft, false);
  else r.removeFromLeft(12);
  g.drawText(text, r, juce::Justification::centredLeft, true);
}

void LookAndFeel::getIdealPopupMenuItemSize(const juce::String &text, bool isSeparator, int, int &idealWidth, int &idealHeight)
{
  if (isSeparator)
  {
    idealWidth = 50;
    idealHeight = 5;
    return;
  }
  idealHeight = (int)ceilf(m_fonts.menu.getHeight()) + 6;
  idealWidth = juce::GlyphArrangement::getStringWidthInt(m_fonts.menu, text) + 12 + 16;
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds(const juce::String &tipText, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
  juce::AttributedString s;
  s.setJustification(juce::Justification::centredLeft);
  s.append(tipText, m_fonts.chat, Colors::value());
  juce::TextLayout layout;
  layout.createLayout(s, 400.0f);
  const int w = (int)ceilf(layout.getWidth()) + 14, h = (int)ceilf(layout.getHeight()) + 8;
  return juce::Rectangle<int>(screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                              screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
    .constrainedWithin(parentArea);
}

void LookAndFeel::drawTooltip(juce::Graphics &g, const juce::String &text, int width, int height)
{
  const juce::Rectangle<float> r(0, 0, (float)width, (float)height);
  g.setColour(findColour(juce::TooltipWindow::backgroundColourId));
  g.fillRoundedRectangle(r, RADIUS);
  g.setColour(findColour(juce::TooltipWindow::outlineColourId));
  g.drawRoundedRectangle(r.reduced(0.5f), RADIUS, 1.0f);
  juce::AttributedString s;
  s.setJustification(juce::Justification::centredLeft);
  s.append(text, m_fonts.chat, findColour(juce::TooltipWindow::textColourId));
  juce::TextLayout layout;
  layout.createLayout(s, (float)width - 14);
  layout.draw(g, r.reduced(7, 4));
}

void LookAndFeel::drawScrollbar(juce::Graphics &g, juce::ScrollBar &, int x, int y, int width, int height, bool isVertical, int thumbStart,
                                int thumbSize, bool, bool)
{
  juce::Rectangle<int> thumb = isVertical ? juce::Rectangle<int>(x, thumbStart, width, thumbSize) : juce::Rectangle<int>(thumbStart, y, thumbSize, height);
  g.setColour(Colors::separator());
  g.fillRect(thumb.reduced(1));
}

// ---------------------------------------------------------------------------

juce::Label *makeLabel(const std::string &text, const juce::Font &font, juce::Colour color, juce::Justification align)
{
  juce::Label *l = new juce::Label(juce::String(), str(text));
  l->setFont(font);
  l->setColour(juce::Label::textColourId, color);
  l->setJustificationType(align);
  l->setBorderSize(juce::BorderSize<int>(0));
  l->setMinimumHorizontalScale(1.0f);
  l->setInterceptsMouseClicks(false, false);
  return l;
}

juce::TextEditor *makeEdit(const juce::Font &font, const juce::String &placeholder)
{
  juce::TextEditor *e = new juce::TextEditor;
  e->setMultiLine(false);
  e->setReturnKeyStartsNewLine(false);
  e->setScrollbarsShown(false);
  e->setFont(font);
  e->setJustification(juce::Justification::centredLeft);
  e->setIndents(5, 0);
  e->setBorder(juce::BorderSize<int>(1));
  e->setColour(juce::TextEditor::textColourId, Colors::value());
  e->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
  e->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
  e->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
  e->setTextToShowWhenEmpty(placeholder, Colors::placeholder());
  e->setSelectAllWhenFocused(false);
  return e;
}

} // namespace ui
} // namespace jamsa
