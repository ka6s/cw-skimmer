/**
 * @file multichanneldecoder.cpp
 * @brief Parallel CW decode — Threshold or Mask backends
 */

#include "multichanneldecoder.h"
#include "thresholdmorsewindow.h"
#include "maskmorsewindow.h"
#include "spectrumwidget.h"

#include <algorithm>
#include <cmath>

const float MultiChannelDecoder::kMinSnrDb = 4.0f;

MultiChannelDecoder::MultiChannelDecoder(QObject *parent)
    : QObject(parent)
    , m_centerHz(0.0f)
    , m_halfBins(1)
    , m_maxActive(1)
    , m_backend(Backend::Threshold)
    , m_lastEmitMs(0)
{
    for (int i = 0; i < kMaxChannels; ++i) {
        m_channels[i].active = false;
        m_channels[i].thrDecoder = nullptr;
        m_channels[i].maskDecoder = nullptr;
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
}

void MultiChannelDecoder::clear()
{
    for (int i = 0; i < kMaxChannels; ++i) {
        destroyChannelEngines(m_channels[i]);
        m_channels[i].active = false;
        resetChannelState(m_channels[i]);
    }
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

    int activeCount = 0;
    for (int i = 0; i < kMaxChannels; ++i) {
        if (m_channels[i].active) {
            activeCount++;
        }
    }
    int guard = kMaxChannels + 1;
    while (activeCount > m_maxActive && guard-- > 0) {
        int weakest = -1;
        for (int i = 0; i < kMaxChannels; ++i) {
            if (!m_channels[i].active) {
                continue;
            }
            if (weakest < 0 || m_channels[i].snrDb < m_channels[weakest].snrDb) {
                weakest = i;
            }
        }
        if (weakest < 0) {
            break;
        }
        destroyChannelEngines(m_channels[weakest]);
        m_channels[weakest].active = false;
        resetChannelState(m_channels[weakest]);
        activeCount--;
    }
    emitSnapshot();
}

QString MultiChannelDecoder::channelText(const Channel &ch) const
{
    if (m_backend == Backend::Mask && ch.maskDecoder) {
        return ch.maskDecoder->trailingText(kDisplayChars);
    }
    if (ch.thrDecoder) {
        return ch.thrDecoder->trailingText(kDisplayChars);
    }
    return QString();
}

QVector<MultiChannelDecoder::ChannelView> MultiChannelDecoder::channels() const
{
    QVector<ChannelView> out;
    out.reserve(kMaxChannels);
    for (int i = 0; i < kMaxChannels; ++i) {
        if (!m_channels[i].active) {
            continue;
        }
        if (m_backend == Backend::Threshold && !m_channels[i].thrDecoder) {
            continue;
        }
        if (m_backend == Backend::Mask && !m_channels[i].maskDecoder) {
            continue;
        }
        ChannelView v;
        v.freqOffsetHz = m_channels[i].offsetHz;
        v.frequencyHz = m_channels[i].frequencyHz;
        v.snrDb = m_channels[i].snrDb;
        v.text = channelText(m_channels[i]);
        v.active = true;
        out.append(v);
    }
    return out;
}

QVector<MultiChannelDecoder::PeakCand> MultiChannelDecoder::findTopPeaks(
    const QVector<float> &spectrum, float binWidth, float noiseFloorDb, int maxPeaks) const
{
    QVector<PeakCand> peaks;
    const int n = spectrum.size();
    if (n < 8 || binWidth <= 0.0f || maxPeaks <= 0) {
        return peaks;
    }

    const float gate = noiseFloorDb + kMinSnrDb;
    const int half = n / 2;
    const int skipEdge = 2;

    for (int b = skipEdge; b < n - skipEdge; ++b) {
        const float p = spectrum[b];
        if (p < gate) {
            continue;
        }
        if (p < spectrum[b - 1] || p < spectrum[b + 1]) {
            continue;
        }
        if (p < spectrum[b - 2] || p < spectrum[b + 2]) {
            continue;
        }

        PeakCand c;
        c.offsetHz = (static_cast<float>(b) - static_cast<float>(half)) * binWidth;
        c.powerDb = p;
        c.snrDb = p - noiseFloorDb;
        peaks.append(c);
    }

    std::sort(peaks.begin(), peaks.end(),
              [](const PeakCand &a, const PeakCand &b) { return a.snrDb > b.snrDb; });

    QVector<PeakCand> filtered;
    for (const PeakCand &p : peaks) {
        bool near = false;
        for (const PeakCand &k : filtered) {
            if (std::fabs(k.offsetHz - p.offsetHz) < static_cast<float>(kMinPeakSeparationHz)) {
                near = true;
                break;
            }
        }
        if (!near) {
            filtered.append(p);
            if (filtered.size() >= maxPeaks) {
                break;
            }
        }
    }
    return filtered;
}

int MultiChannelDecoder::matchChannel(float offsetHz) const
{
    int best = -1;
    float bestDist = static_cast<float>(kMatchHz);
    for (int i = 0; i < kMaxChannels; ++i) {
        if (!m_channels[i].active) {
            continue;
        }
        const float d = std::fabs(m_channels[i].offsetHz - offsetHz);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

int MultiChannelDecoder::allocateChannel(float offsetHz, float snrDb)
{
    int activeCount = 0;
    for (int i = 0; i < kMaxChannels; ++i) {
        if (m_channels[i].active) {
            activeCount++;
        }
    }

    auto setupEngines = [this](Channel &ch) {
        destroyChannelEngines(ch);
        if (m_backend == Backend::Mask) {
            ch.maskDecoder = new MaskMorseWindow(nullptr, true);
            ch.maskDecoder->setPlaybackMode(true);
            ch.maskDecoder->clearDecode();
            ch.maskDecoder->resetTiming();
        } else {
            ch.thrDecoder = new ThresholdMorseWindow(nullptr, true);
            ch.thrDecoder->setPlaybackMode(true);
            ch.thrDecoder->clearDecode();
            ch.thrDecoder->resetTiming();
        }
    };

    if (activeCount < m_maxActive) {
        for (int i = 0; i < kMaxChannels; ++i) {
            if (!m_channels[i].active) {
                Channel &ch = m_channels[i];
                resetChannelState(ch);
                ch.active = true;
                ch.offsetHz = offsetHz;
                ch.snrDb = snrDb;
                setupEngines(ch);
                return i;
            }
        }
    }

    int weakest = -1;
    for (int i = 0; i < kMaxChannels; ++i) {
        if (!m_channels[i].active) {
            continue;
        }
        if (weakest < 0 || m_channels[i].snrDb < m_channels[weakest].snrDb) {
            weakest = i;
        }
    }
    if (weakest >= 0 && snrDb > m_channels[weakest].snrDb + 1.5f) {
        Channel &ch = m_channels[weakest];
        resetChannelState(ch);
        ch.active = true;
        ch.offsetHz = offsetHz;
        ch.snrDb = snrDb;
        setupEngines(ch);
        return weakest;
    }
    return -1;
}

void MultiChannelDecoder::dropStaleChannels(qint64 nowMs)
{
    for (int i = 0; i < kMaxChannels; ++i) {
        if (!m_channels[i].active) {
            continue;
        }
        if (nowMs - m_channels[i].lastSeenMs > kChannelTimeoutMs) {
            destroyChannelEngines(m_channels[i]);
            m_channels[i].active = false;
            resetChannelState(m_channels[i]);
        }
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
    const qint64 nowMs = m_clock.elapsed();
    const QVector<PeakCand> peaks =
        findTopPeaks(spectrum, binWidth, noiseFloorDb, m_maxActive);

    for (const PeakCand &pk : peaks) {
        int idx = matchChannel(pk.offsetHz);
        if (idx < 0) {
            idx = allocateChannel(pk.offsetHz, pk.snrDb);
        }
        if (idx < 0) {
            continue;
        }
        Channel &ch = m_channels[idx];
        ch.offsetHz = 0.85f * ch.offsetHz + 0.15f * pk.offsetHz;
        ch.frequencyHz = centerHz + ch.offsetHz;
        ch.snrDb = 0.8f * ch.snrDb + 0.2f * pk.snrDb;
        ch.lastSeenMs = nowMs;
    }

    dropStaleChannels(nowMs);

    for (int i = 0; i < kMaxChannels; ++i) {
        if (!m_channels[i].active) {
            continue;
        }
        Channel &ch = m_channels[i];
        const float powerDb =
            SpectrumWidget::powerAtOffset(spectrum, ch.offsetHz, binWidth, m_halfBins);
        ch.lastPowerDb = powerDb;
        feedChannel(ch, powerDb, noiseFloorDb, nowMs);
    }

    if (nowMs - m_lastEmitMs >= 60) {
        m_lastEmitMs = nowMs;
        emitSnapshot();
    }
}
