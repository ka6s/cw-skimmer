/**
 * @file multichanneldecoder.cpp
 * @brief Parallel CW decode — Spectrum, Threshold, or Mask backends
 */

#include "multichanneldecoder.h"
#include "thresholdmorsewindow.h"
#include "maskmorsewindow.h"
#include "spectrummorsewindow.h"
#include "spectrumwidget.h"

#include <algorithm>
#include <cmath>

const float MultiChannelDecoder::kMinSnrDb = 16.0f;

MultiChannelDecoder::MultiChannelDecoder(QObject *parent)
    : QObject(parent)
    , m_centerHz(0.0f)
    , m_halfBins(1)
    , m_maxActive(kMaxChannels)
    , m_backend(Backend::Spectrum)
    , m_columnClockMs(-1)
    , m_lastEmitMs(0)
    , m_columnSerial(0)
    , m_naturalSlots(0)
    , m_spanHz(0.0f)
{
    for (int i = 0; i < kMaxChannels; ++i) {
        m_channels[i].active = false;
        m_channels[i].thrDecoder = nullptr;
        m_channels[i].maskDecoder = nullptr;
        m_channels[i].scopeDecoder = nullptr;
        resetChannelState(m_channels[i]);
    }
    m_clock.start();
}

MultiChannelDecoder::~MultiChannelDecoder()
{
    clear();
}

void MultiChannelDecoder::resetChannelState(Channel &ch)
{
    ch.trackInit = false;
    ch.keyHigh = false;
    ch.thrDb = -80.0f;
    ch.trackLowDb = -90.0f;
    ch.trackHighDb = -70.0f;
    ch.noisePeakDb = -200.0f;
    ch.markPeakHoldDb = -200.0f;
    ch.markBodyDb = -200.0f;
    ch.noiseEmaDb = -95.0f;
    ch.lastPowerDb = -200.0f;
    ch.lastSeenMs = 0;
    ch.offsetHz = 0.0f;
    ch.frequencyHz = 0.0f;
    ch.snrDb = 0.0f;
    ch.sustained = false;
    ch.bornSerial = -1;
    ch.switchHz = 0.0f;
    ch.switchHits = 0;
    ch.powerHist.clear();
}

void MultiChannelDecoder::destroyChannelEngines(Channel &ch)
{
    if (ch.thrDecoder) {
        delete ch.thrDecoder;
        ch.thrDecoder = nullptr;
    }
    if (ch.maskDecoder) {
        delete ch.maskDecoder;
        ch.maskDecoder = nullptr;
    }
    if (ch.scopeDecoder) {
        delete ch.scopeDecoder;
        ch.scopeDecoder = nullptr;
    }
}

void MultiChannelDecoder::clear()
{
    for (int i = 0; i < kMaxChannels; ++i) {
        destroyChannelEngines(m_channels[i]);
        m_channels[i].active = false;
        resetChannelState(m_channels[i]);
    }
    m_spanHz = 0.0f;
    m_naturalSlots = 0;
    emitSnapshot();
}

bool MultiChannelDecoder::setBackend(Backend backend, QString *errorOut)
{
    (void)errorOut;
    if (backend == m_backend) {
        return true;
    }
    clear();
    m_backend = backend;
    return true;
}

void MultiChannelDecoder::setHalfBins(int halfBins)
{
    m_halfBins = std::max(0, std::min(16, halfBins));
}

void MultiChannelDecoder::setMaxActiveChannels(int n)
{
    if (n < 1) {
        n = 1;
    }
    if (n > kMaxChannels) {
        n = kMaxChannels;
    }
    m_maxActive = n;
    emitSnapshot();
}

QString MultiChannelDecoder::channelText(const Channel &ch) const
{
    /* Full decoder buffer, so CQ and a repeated call can be recolored
     * anywhere in the text the side window scrolls back through. */
    if (ch.scopeDecoder) {
        return ch.scopeDecoder->decodedText();
    }
    return QString();
}

QVector<MultiChannelDecoder::ChannelView> MultiChannelDecoder::channels() const
{
    QVector<ChannelView> out;
    out.reserve(kMaxChannels);
    for (int i = 0; i < kMaxChannels; ++i) {
        if (!m_channels[i].active || !slotEnabled(i)) {
            continue;
        }
        if (!m_channels[i].scopeDecoder) {
            continue;
        }
        ChannelView v;
        v.slot = i;
        v.freqOffsetHz = m_channels[i].offsetHz;
        v.frequencyHz = m_channels[i].frequencyHz;
        v.snrDb = m_channels[i].snrDb;
        v.text = channelText(m_channels[i]);
        v.active = true;
        out.append(v);
    }
    return out;
}

QString MultiChannelDecoder::textForOffset(float offsetHz) const
{
    if (!(m_spanHz > 1.0f)) {
        return QString();
    }
    const int slot = slotForOffset(offsetHz, m_spanHz);
    if (slot < 0 || slot >= kMaxChannels) {
        return QString();
    }
    const Channel &ch = m_channels[slot];
    if (!ch.active || !ch.scopeDecoder) {
        return QString();
    }
    return ch.scopeDecoder->decodedText();
}

int MultiChannelDecoder::slotCountForSpan(float spanHz)
{
    if (!(spanHz > 1.0f)) {
        return 1;
    }
    int n = static_cast<int>(std::lround(static_cast<double>(spanHz) / static_cast<double>(kSlotHz)));
    if (n < 1) {
        n = 1;
    }
    if (n > kMaxChannels) {
        n = kMaxChannels;
    }
    return n;
}

int MultiChannelDecoder::slotForOffset(float offsetHz, float spanHz)
{
    const int n = slotCountForSpan(spanHz);
    const double slotHz = static_cast<double>(spanHz) / static_cast<double>(n);
    const double low = -static_cast<double>(spanHz) / 2.0;
    int slot = static_cast<int>(std::floor((static_cast<double>(offsetHz) - low) / slotHz));
    if (slot < 0) {
        slot = 0;
    }
    if (slot >= n) {
        slot = n - 1;
    }
    return slot;
}

bool MultiChannelDecoder::slotEnabled(int slot) const
{
    if (slot < 0 || slot >= m_naturalSlots) {
        return false;
    }
    const int used = std::min(m_naturalSlots, std::max(1, m_maxActive));
    const int first = (m_naturalSlots - used) / 2;
    return slot >= first && slot < first + used;
}

bool MultiChannelDecoder::findStrongestPeak(const QVector<float> &spectrum, float binWidth,
                                            float noiseFloorDb, float lowHz, float highHz,
                                            PeakCand &out) const
{
    const int n = spectrum.size();
    if (n < 8 || binWidth <= 0.0f || !(highHz > lowHz)) {
        return false;
    }

    const float gate = noiseFloorDb + kMinSnrDb;
    const int half = n / 2;
    bool found = false;
    for (int b = 2; b < n - 2; ++b) {
        const float p = spectrum[b];
        if (p < gate) {
            continue;
        }
        /* A tone split equally across two bins is still the peak. A flat
         * stretch of grass is not: the bin has to rise above one neighbor. */
        if (p < spectrum[b - 1] || p < spectrum[b + 1]) {
            continue;
        }
        if (p < spectrum[b - 2] || p < spectrum[b + 2]) {
            continue;
        }
        if (p <= spectrum[b - 1] && p <= spectrum[b + 1]) {
            continue;
        }

        const float offset = (static_cast<float>(b) - static_cast<float>(half)) * binWidth;
        if (offset < lowHz || offset >= highHz) {
            continue;
        }
        const float snr = p - noiseFloorDb;
        if (!found || snr > out.snrDb) {
            out.offsetHz = offset;
            out.powerDb = p;
            out.snrDb = snr;
            found = true;
        }
    }
    return found;
}

void MultiChannelDecoder::armSlot(Channel &ch, const PeakCand &pk, qint64 nowMs)
{
    if (!ch.scopeDecoder) {
        ch.scopeDecoder = new SpectrumMorseWindow(nullptr, true);
    }
    ch.scopeDecoder->clearDecode();
    ch.scopeDecoder->resetTiming();
    ch.active = true;
    ch.offsetHz = pk.offsetHz;
    ch.snrDb = pk.snrDb;
    ch.switchHz = pk.offsetHz;
    ch.switchHits = 0;
    ch.sustained = false;
    ch.bornSerial = m_columnSerial;
    ch.lastSeenMs = nowMs;
}

void MultiChannelDecoder::retargetSlot(Channel &ch, const PeakCand &pk, qint64 nowMs)
{
    if (ch.scopeDecoder) {
        const QString kept = ch.scopeDecoder->decodedText();
        ch.scopeDecoder->adoptText(kept);
    }
    ch.offsetHz = pk.offsetHz;
    ch.snrDb = pk.snrDb;
    ch.switchHz = pk.offsetHz;
    ch.switchHits = 0;
    ch.sustained = false;
    ch.bornSerial = m_columnSerial;
    ch.lastSeenMs = nowMs;
}

void MultiChannelDecoder::trackSlot(Channel &ch, const QVector<float> &spectrum, float binWidth,
                                    float noiseFloorDb, float slotLow, float slotHigh,
                                    qint64 nowMs)
{
    PeakCand best;
    const bool haveBest = findStrongestPeak(spectrum, binWidth, noiseFloorDb,
                                            slotLow, slotHigh, best);
    if (!ch.active) {
        if (haveBest) {
            armSlot(ch, best, nowMs);
        }
        return;
    }

    const float nearLow = std::max(slotLow, ch.offsetHz - kFollowHz);
    const float nearHigh = std::min(slotHigh, ch.offsetHz + kFollowHz + 1.0f);
    PeakCand near;
    const bool haveNear = findStrongestPeak(spectrum, binWidth, noiseFloorDb,
                                            nearLow, nearHigh, near);
    if (haveNear && (!haveBest
                     || std::fabs(best.offsetHz - ch.offsetHz) <= kFollowHz
                     || near.snrDb + kSwitchMarginDb >= best.snrDb)) {
        ch.offsetHz = near.offsetHz;
        ch.snrDb = 0.8f * ch.snrDb + 0.2f * near.snrDb;
        ch.switchHits = 0;
        ch.lastSeenMs = nowMs;
        if (ch.bornSerial != m_columnSerial) {
            ch.sustained = true;
        }
        return;
    }

    /* Nothing else is in this slice. Leave the lock and the copy where
     * they are. A new peak has to hold for two columns, below, and the
     * copy already decoded stays when the slice moves. */
    if (!haveBest || std::fabs(best.offsetHz - ch.offsetHz) <= kFollowHz) {
        ch.switchHits = 0;
        return;
    }
    if (haveNear && !(best.snrDb > near.snrDb + kSwitchMarginDb)) {
        ch.switchHits = 0;
        return;
    }

    if (ch.switchHits > 0 && std::fabs(best.offsetHz - ch.switchHz) <= kFollowHz) {
        ch.switchHits++;
    } else {
        ch.switchHits = 1;
        ch.switchHz = best.offsetHz;
    }
    if (ch.switchHits >= kSwitchColumns) {
        retargetSlot(ch, best, nowMs);
    }
}

void MultiChannelDecoder::updateChannelThreshold(Channel &ch, float powerDb, float noiseFloorDb)
{
    /* Same dual-rail Auto thr as Signal Trace (noise peak floor + mark peak) */
    ch.powerHist.append(powerDb);
    while (ch.powerHist.size() > kThreshHistMax) {
        ch.powerHist.removeFirst();
    }

    if (noiseFloorDb > -180.0f) {
        if (!ch.trackInit) {
            ch.noiseEmaDb = noiseFloorDb;
        } else {
            ch.noiseEmaDb = 0.995f * ch.noiseEmaDb + 0.005f * noiseFloorDb;
        }
    }

    if (ch.powerHist.size() < 16) {
        float lo = ch.powerHist.first();
        float hi = ch.powerHist.first();
        for (float p : ch.powerHist) {
            lo = std::min(lo, p);
            hi = std::max(hi, p);
        }
        if (hi - lo >= 5.0f) {
            const float early = lo + 0.55f * (hi - lo);
            ch.thrDb = ch.trackInit ? (0.85f * ch.thrDb + 0.15f * early) : early;
        } else {
            ch.thrDb = std::max(ch.thrDb, ch.noiseEmaDb + 6.0f);
        }
        ch.trackLowDb = lo;
        ch.trackHighDb = hi;
        ch.trackInit = true;
        return;
    }

    QVector<float> sorted = ch.powerHist;
    std::sort(sorted.begin(), sorted.end());
    const int n = sorted.size();
    auto percentile = [&](float pct) -> float {
        const float idx = pct * 0.01f * static_cast<float>(n - 1);
        const int i0 = static_cast<int>(idx);
        const int i1 = std::min(n - 1, i0 + 1);
        const float f = idx - static_cast<float>(i0);
        return sorted[i0] * (1.0f - f) + sorted[i1] * f;
    };

    const float p10 = percentile(10.0f);
    const float p20 = percentile(20.0f);
    const float p40 = percentile(40.0f);
    const float p50 = percentile(50.0f);
    const float spaceDb = 0.5f * (p10 + p20);

    float localNoisePeak = -200.0f;
    for (float p : ch.powerHist) {
        if (p <= p40 + 1.0f) {
            localNoisePeak = std::max(localNoisePeak, p);
        }
    }
    if (localNoisePeak > -180.0f) {
        if (ch.noisePeakDb < -180.0f) {
            ch.noisePeakDb = localNoisePeak;
        } else if (localNoisePeak >= ch.noisePeakDb) {
            ch.noisePeakDb = 0.80f * ch.noisePeakDb + 0.20f * localNoisePeak;
        } else {
            ch.noisePeakDb -= 0.002f;
            ch.noisePeakDb = std::max(ch.noisePeakDb, localNoisePeak);
        }
    }

    const float markGate = (ch.noisePeakDb > -180.0f)
                               ? (ch.noisePeakDb + 5.0f)
                               : (spaceDb + 6.5f);
    const bool isMark = powerDb >= markGate;

    if (isMark) {
        if (ch.markPeakHoldDb < -180.0f || powerDb > ch.markPeakHoldDb) {
            ch.markPeakHoldDb = powerDb;
        }
        if (ch.markBodyDb < -180.0f) {
            ch.markBodyDb = powerDb;
        } else if (powerDb >= ch.markBodyDb) {
            ch.markBodyDb = 0.88f * ch.markBodyDb + 0.12f * powerDb;
        } else {
            ch.markBodyDb = 0.985f * ch.markBodyDb + 0.015f * powerDb;
        }
    } else if (ch.markPeakHoldDb > -180.0f) {
        ch.markPeakHoldDb -= 0.0004f;
        if (ch.noisePeakDb > -180.0f && ch.markPeakHoldDb < ch.noisePeakDb + 4.0f) {
            ch.markPeakHoldDb = -200.0f;
        }
    }

    float markDb = ch.markBodyDb;
    if (markDb < -180.0f && ch.markPeakHoldDb > -180.0f) {
        markDb = ch.markPeakHoldDb;
        ch.markBodyDb = markDb;
    }
    if (markDb < -180.0f) {
        markDb = std::max(p50 + 5.0f, spaceDb + 9.0f);
    }

    ch.trackLowDb = (ch.noisePeakDb > -180.0f) ? ch.noisePeakDb : spaceDb;
    ch.trackHighDb = (ch.markPeakHoldDb > -180.0f) ? ch.markPeakHoldDb : markDb;
    ch.trackInit = true;

    float thrFloor = (ch.noisePeakDb > -180.0f) ? (ch.noisePeakDb + 6.5f) : (spaceDb + 8.0f);
    float thrCeil = (ch.markPeakHoldDb > -180.0f) ? (ch.markPeakHoldDb - 3.5f) : (markDb - 3.5f);
    thrFloor = std::max(thrFloor, ch.noiseEmaDb + 6.0f);
    if (thrCeil < thrFloor + 2.0f) {
        thrCeil = thrFloor + 2.0f;
    }

    float ideal = thrFloor + 0.40f * (thrCeil - thrFloor);
    ideal = std::max(ideal, thrFloor);
    ideal = std::min(ideal, thrCeil);

    const float err = ideal - ch.thrDb;
    if (err > 2.5f) {
        ch.thrDb = 0.70f * ch.thrDb + 0.30f * ideal;
    } else if (err > 0.0f) {
        ch.thrDb = 0.92f * ch.thrDb + 0.08f * ideal;
    } else {
        ch.thrDb = 0.9985f * ch.thrDb + 0.0015f * ideal;
    }

    ch.thrDb = std::max(ch.thrDb, thrFloor);
    ch.thrDb = std::min(ch.thrDb, thrCeil);
    ch.thrDb = std::min(40.0f, std::max(-130.0f, ch.thrDb));
}

void MultiChannelDecoder::feedChannel(Channel &ch, float powerDb, float noiseFloorDb, qint64 nowMs)
{
    updateChannelThreshold(ch, powerDb, noiseFloorDb);

    const float hyst = 2.0f;
    bool above = ch.keyHigh;
    if (ch.keyHigh) {
        if (powerDb < ch.thrDb - hyst) {
            above = false;
        }
    } else {
        if (powerDb > ch.thrDb + hyst * 0.4f) {
            above = true;
        }
    }
    ch.keyHigh = above;

    if (m_backend == Backend::Mask && ch.maskDecoder) {
        ch.maskDecoder->setThresholdDb(ch.thrDb);
        ch.maskDecoder->setNoiseFloorDb(noiseFloorDb);
        ch.maskDecoder->feedSample(powerDb, above, nowMs);
    } else if (ch.thrDecoder) {
        ch.thrDecoder->setThresholdDb(ch.thrDb);
        ch.thrDecoder->feedSample(powerDb, above, nowMs);
    }
}

void MultiChannelDecoder::emitSnapshot()
{
    emit channelsUpdated(channels());
}

void MultiChannelDecoder::processSpectrumColumn(const QVector<float> &spectrum, float binWidth,
                                                float centerHz, float noiseFloorDb)
{
    if (spectrum.isEmpty() || binWidth <= 0.0f) {
        return;
    }

    m_centerHz = centerHz;
    /* Wideband hop is one FFT (period = 1/binWidth). Narrow hops ~10.7 ms. */
    const double periodMs = (binWidth > 20.0f) ? (1000.0 / binWidth) : 10.7;
    if (m_columnClockMs < 0) {
        m_columnClockMs = 0;
    }
    const qint64 nowMs = m_columnClockMs;
    m_columnClockMs += static_cast<qint64>(std::llround(periodMs));
    ++m_columnSerial;

    const float span = static_cast<float>(spectrum.size()) * binWidth;
    if (m_spanHz > 0.0f && std::fabs(m_spanHz - span) > 1.0f) {
        for (int i = 0; i < kMaxChannels; ++i) {
            if (m_channels[i].scopeDecoder) {
                m_channels[i].scopeDecoder->clearDecode();
                m_channels[i].scopeDecoder->resetTiming();
            }
            m_channels[i].active = false;
            resetChannelState(m_channels[i]);
        }
    }
    m_spanHz = span;
    m_naturalSlots = slotCountForSpan(span);

    const int used = std::min(m_naturalSlots, std::max(1, m_maxActive));
    const int first = (m_naturalSlots - used) / 2;
    const float slotHz = span / static_cast<float>(m_naturalSlots);
    const float bandLow = -span / 2.0f;

    for (int slot = first; slot < first + used; ++slot) {
        const float slotLow = bandLow + static_cast<float>(slot) * slotHz;
        const float slotHigh = slotLow + slotHz;
        Channel &ch = m_channels[slot];
        trackSlot(ch, spectrum, binWidth, noiseFloorDb, slotLow, slotHigh, nowMs);
        if (!ch.active) {
            continue;
        }
        ch.frequencyHz = centerHz + ch.offsetHz;
        const float powerDb =
            SpectrumWidget::powerAtOffset(spectrum, ch.offsetHz, binWidth, m_halfBins);
        ch.lastPowerDb = powerDb;
        if (!ch.scopeDecoder) {
            ch.scopeDecoder = new SpectrumMorseWindow(nullptr, true);
            ch.scopeDecoder->clearDecode();
            ch.scopeDecoder->resetTiming();
        }
        ch.scopeDecoder->feedColumn(powerDb, noiseFloorDb, binWidth);
    }

    if (nowMs - m_lastEmitMs >= 60) {
        m_lastEmitMs = nowMs;
        emitSnapshot();
    }
}
