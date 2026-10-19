/*
    O11 Session - click.cpp
    Copyright (C) 2026 and onward, O11 Session contributors

    Part of the O11 Session plug-in on JUCE. The licence is decided at release (see README.md).
*/

#include "click.h"

#include <math.h>

#include <algorithm>

namespace o11 {

static const double PI = 3.14159265358979323846;

int Click::length(double sampleRate)
{
  return sampleRate > 0 ? (int)ceil(LENGTH * sampleRate) : 0;
}

double Click::sample(bool accent, int n, double sampleRate)
{
  if (n < 0 || sampleRate <= 0) return 0;
  const double t = n / sampleRate;
  if (t >= LENGTH) return 0;
  // the attack: a raised cosine from 0 to 1
  const double attack = t < ATTACK ? 0.5 * (1.0 - cos(PI * t / ATTACK)) : 1.0;
  // the decay: exponential, to -60 dB at SIXTY_DB, less what is left at LENGTH, so it ends at 0
  const double tau = (SIXTY_DB - ATTACK) / log(1000.0);
  const double left = exp(-(LENGTH - ATTACK) / tau);
  const double decay = t <= ATTACK ? 1.0 : (exp(-(t - ATTACK) / tau) - left) / (1.0 - left);
  const double hz = accent ? ACCENT_HZ : BEAT_HZ, level = accent ? 1.0 : BEAT_LEVEL;
  return level * attack * decay * sin(2.0 * PI * hz * t);
}

void Click::setSampleRate(double sampleRate)
{
  if (sampleRate <= 0) return;
  m_n = -1;
  if (sampleRate == m_sampleRate && m_samples[0].size() == (size_t)m_length) return; // made already
  m_sampleRate = sampleRate;
  m_length = length(sampleRate);
  for (int accent = 0; accent < 2; accent ++)
  {
    m_samples[accent].resize((size_t)m_length);
    for (int n = 0; n < m_length; n ++) m_samples[accent][(size_t)n] = sample(accent != 0, n, sampleRate);
  }
}

void Click::reset()
{
  m_n = -1;
}

// where beat k starts in an interval of len samples and bpi beats
static long long beat_start(int k, int len, int bpi)
{
  return ((long long)k * len * 2 + bpi) / ((long long)bpi * 2); // k * len / bpi, rounded
}

void Click::render(float *left, float *right, int frames, int start, int len, int bpi, float volume)
{
  if (!left || frames <= 0) return;
  // the next beat at or after `start`
  int k = 0;
  long long next = -1;
  if (len > 0 && bpi > 0)
  {
    if (start > 0)
    {
      k = (int)((long long)start * bpi / len);
      while (k > 0 && beat_start(k - 1, len, bpi) >= start) k--;
      while (k < bpi && beat_start(k, len, bpi) < start) k++;
    }
    if (k < bpi) next = beat_start(k, len, bpi);
  }

  // a stretch at a time: up to the next beat, or the end of the click playing
  for (int i = 0; i < frames; )
  {
    if (start + i == next)
    {
      m_n = 0;
      m_accent = k == 0;
      next = ++k < bpi ? beat_start(k, len, bpi) : -1;
    }
    int until = frames;
    const long long beat = next - start;
    if (beat > i && beat < until) until = (int)beat;
    if (m_n < 0)
    {
      i = until; // silence until then
      continue;
    }
    const int count = std::min(until - i, m_length - m_n);
    if (volume > 0)
    {
      const std::vector<double> &made = m_samples[m_accent ? 1 : 0];
      for (int j = 0; j < count; j ++)
      {
        const int n = m_n + j;
        const double s = (size_t)n < made.size() ? made[(size_t)n] : sample(m_accent, n, m_sampleRate);
        const float v = (float)(s * volume);
        left[i + j] += v;
        if (right) right[i + j] += v;
      }
    }
    m_n += count;
    i += count;
    if (m_n >= m_length) m_n = -1;
  }
}

} // namespace o11
